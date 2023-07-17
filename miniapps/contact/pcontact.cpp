//                                Contact example
//
// Compile with: make contact
//
// Sample runs:  ./contact -m1 block1.mesh -m2 block2.mesh -at "5 6 7 8"
// Sample runs:  ./contact -m1 block1_d.mesh -m2 block2_d.mesh -at "5 6 7 8"

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include "util/contact_util.hpp"
#include "util/util.hpp"
#include "util/mpicomm.hpp"

using namespace std;
using namespace mfem;


// function to verify correctness of parallel code
// function f(x) = x
void rhs_func1(const Vector & x, Vector & y)
{
   for (int i = 0; i<x.Size(); i++)
   {
      y(i) = sin(x(i));
   }
}

// function f(x) = x.^2
void rhs_func2(const Vector & x, Vector & y)
{
   for (int i = 0; i<x.Size(); i++)
   {
      y(i) = cos(x(i));
   }
}

int get_rank(int tdof, std::vector<int> & tdof_offsets)
{
   int size = tdof_offsets.size();
   if (size == 1) { return 0; }
   std::vector<int>::iterator up;
   up=std::upper_bound(tdof_offsets.begin(), tdof_offsets.end(),tdof); //
   return std::distance(tdof_offsets.begin(),up)-1;
}

void ComputeTdofOffsets(const ParFiniteElementSpace * pfes,
                        std::vector<int> & tdof_offsets)
{
   MPI_Comm comm = pfes->GetComm();
   int num_procs;
   MPI_Comm_size(comm, &num_procs);
   tdof_offsets.resize(num_procs);
   int mytoffset = pfes->GetMyTDofOffset();
   MPI_Allgather(&mytoffset,1,MPI_INT,&tdof_offsets[0],1,MPI_INT,comm);
}

void ComputeTdofOffsets(MPI_Comm comm, int mytoffset, std::vector<int> & tdof_offsets)
{
   int num_procs;
   MPI_Comm_size(comm,&num_procs);
   tdof_offsets.resize(num_procs);
   MPI_Allgather(&mytoffset,1,MPI_INT,&tdof_offsets[0],1,MPI_INT,comm);
}

void ComputeTdofs(MPI_Comm comm, int mytoffs, std::vector<int> & tdofs)
{
   int num_procs;
   MPI_Comm_size(comm,&num_procs);
   tdofs.resize(num_procs);
   MPI_Allgather(&mytoffs,1,MPI_INT,&tdofs,1,MPI_INT,comm);
}

// Coordinates in xyz are assumed to be ordered as [X, Y, Z]
// where X is the list of x-coordinates for all points and so on.
// conn: connectivity of the target surface elements
// xi: surface reference cooridnates for the cloest point, involves a linear transformation from [0,1] to [-1,1]
void FindPointsInMesh(Mesh & mesh, const Array<int> & gvert, const Vector & xyz, const Array<int> & s_conn, Array<int>& conn,
                      Vector & xyz2, Array<int> & s_conn2, Vector& xi, DenseMatrix & coords)
{
   const int dim = mesh.Dimension();
   const int np = xyz.Size() / dim;

   MFEM_VERIFY(np * dim == xyz.Size(), "");

   mesh.EnsureNodes();

   FindPointsGSLIB finder(MPI_COMM_WORLD);

   finder.SetDistanceToleranceForPointsFoundOnBoundary(0.5);

   const double bb_t = 0.5;
   finder.Setup(mesh, bb_t);

   finder.FindPoints(xyz);

   Array<unsigned int> procs = finder.GetProc();

   /// Return code for each point searched by FindPoints: inside element (0), on
   /// element boundary (1), or not found (2).
   Array<unsigned int> codes = finder.GetCode();

   /// Return element number for each point found by FindPoints.
   Array<unsigned int> elems = finder.GetElem();

   /// Return reference coordinates for each point found by FindPoints.
   Vector refcrd = finder.GetReferencePosition();

   /// Return distance between the sought and the found point in physical space,
   /// for each point found by FindPoints.
   Vector dist = finder.GetDist();

   MFEM_VERIFY(dist.Size() == np, "");
   MFEM_VERIFY(refcrd.Size() == np * dim, "");
   MFEM_VERIFY(elems.Size() == np, "");
   MFEM_VERIFY(codes.Size() == np, "");

   bool allfound = true;
   for (auto code : codes)
      if (code == 2) { allfound = false; }

   MFEM_VERIFY(allfound, "A point was not found");

   // cout << "Maximum distance of projected points: " << dist.Max() << endl;


   Array<unsigned int> elems_recv, proc_recv;
   Vector ref_recv;
   Vector xyz_recv;
   Array<int> s_conn_recv;

   MPICommunicator mycomm(MPI_COMM_WORLD, procs);
   mycomm.Communicate(xyz,xyz_recv,3,mfem::Ordering::byNODES);
   mycomm.Communicate(elems,elems_recv,1,mfem::Ordering::byVDIM);
   mycomm.Communicate(refcrd,ref_recv,3,mfem::Ordering::byVDIM);
   mycomm.Communicate(s_conn,s_conn_recv,1,mfem::Ordering::byVDIM);

   proc_recv = mycomm.GetOriginProcs();

   int np_loc = elems_recv.Size();
   Array<int> conn_loc(np_loc*4);
   Vector xi_send(np_loc*(dim-1));
   for (int i=0; i<np_loc; ++i)
   {
      int refFace, refNormal, refNormalSide;
      bool is_interior = -1;

      Vector normal = GetNormalVector(mesh, elems_recv[i],
                                      ref_recv.GetData() + (i*dim),
                                      refFace, refNormal, is_interior);

      // continue;
      int phyFace;
      if (is_interior)
      {
         phyFace = -1; // the id of the face that has the closest point
         FindSurfaceToProject(mesh, elems_recv[i], phyFace); // seems that this works

         Array<int> cbdrVert;
         mesh.GetFaceVertices(phyFace, cbdrVert);
         Vector xs(dim);
         xs[0] = xyz_recv[i + 0*np_loc];
         xs[1] = xyz_recv[i + 1*np_loc];
         xs[2] = xyz_recv[i + 2*np_loc];

         Vector xi_tmp(dim-1);
         // get nodes!

         GridFunction *nodes = mesh.GetNodes();
         DenseMatrix coords(4,3);
         for (int i=0; i<4; i++)
         {
            for (int j=0; j<3; j++)
            {
               coords(i,j) = (*nodes)[cbdrVert[i]*3+j];
            }
         }
         SlaveToMaster(coords, xs, xi_tmp);

         for (int j=0; j<dim-1; ++j)
         {
            xi_send[i*(dim-1)+j] = xi_tmp[j];
         }
         // now get get the projection to the surface
      }
      else
      {
         Vector faceRefCrd(dim-1);
         {
            int fd = 0;
            for (int j=0; j<dim; ++j)
            {
               if (j == refNormal)
               {
                  refNormalSide = (ref_recv[(i*dim) + j] > 0.5);
               }
               else
               {
                  faceRefCrd[fd] = ref_recv[(i*dim) + j];
                  fd++;
               }
            }
            MFEM_VERIFY(fd == dim-1, "");
         }

         for (int j=0; j<dim-1; ++j)
         {
            xi_send[i*(dim-1)+j] = faceRefCrd[j]*2.0 - 1.0;
         }
      }
      // Get the element face
      Array<int> faces;
      Array<int> ori;
      int face;

      if (is_interior)
      {
         face = phyFace;
      }
      else
      {
         mesh.GetElementFaces(elems_recv[i], faces, ori);
         face = faces[refFace];
      }

      Array<int> faceVert;
      mesh.GetFaceVertices(face, faceVert);

      for (int p=0; p<4; p++)
      {
         conn_loc[4*i+p] = faceVert[p];
      }
   }

   if (0) // for debugging
   {
      int sz = xi_send.Size()/2;

      for (int i = 0; i<sz; i++)
      {
         mfem::out << "("<<xi_send[i*(dim-1)]<<","<<xi_send[i*(dim-1)+1]<<"): -> ";
         for (int j = 0; j<4; j++)
         {
            double * vc = mesh.GetVertex(conn_loc[4*i+j]);
            if (j<3)
            {
               mfem::out << "("<<vc[0]<<","<<vc[1]<<","<<vc[2]<<"), ";
            }
            else
            {
               mfem::out << "("<<vc[0]<<","<<vc[1]<<","<<vc[2]<<") \n " << endl;
            }
         }
      }
   }

   int sz = xi_send.Size()/2;
   DenseMatrix coordsm(sz*4, dim);
   for (int i = 0; i<sz; i++)
   {
      for (int j = 0; j<4; j++)
      {
         for (int k=0; k<dim; k++)
         {
            coordsm(i*4+j,k) = mesh.GetVertex(conn_loc[i*4+j])[k];
         }
      }
   }

   // pass global indices for conn_loc
   for (int i = 0; i<conn_loc.Size(); i++)
   {
      conn_loc[i] = gvert[conn_loc[i]];
   }

   mycomm.UpdateDestinationProcs();
   mycomm.Communicate(xyz_recv,xyz2,3,mfem::Ordering::byNODES);
   mycomm.Communicate(xi_send,xi,2,mfem::Ordering::byVDIM);
   mycomm.Communicate(s_conn_recv,s_conn2,1,mfem::Ordering::byVDIM);
   mycomm.Communicate(conn_loc,conn,4,mfem::Ordering::byVDIM);
   mycomm.Communicate(coordsm,coords,4,mfem::Ordering::byVDIM);
}

void FindPointsInMesh(Mesh & mesh, const Array<int> & gvert, Array<int> & s_conn, const ParGridFunction &x1, Vector & xyz, Array<int>& conn,
                      Vector& xi, DenseMatrix & coords)
{
   const int dim = mesh.Dimension();
   const int np = xyz.Size() / dim;

   MFEM_VERIFY(np * dim == xyz.Size(), "");

   mesh.EnsureNodes();

   FindPointsGSLIB finder(MPI_COMM_WORLD);

   finder.SetDistanceToleranceForPointsFoundOnBoundary(0.5);

   const double bb_t = 0.5;
   finder.Setup(mesh, bb_t);

   finder.FindPoints(xyz);

   Array<unsigned int> procs = finder.GetProc();

   /// Return code for each point searched by FindPoints: inside element (0), on
   /// element boundary (1), or not found (2).
   Array<unsigned int> codes = finder.GetCode();

   /// Return element number for each point found by FindPoints.
   Array<unsigned int> elems = finder.GetElem();

   /// Return reference coordinates for each point found by FindPoints.
   Vector refcrd = finder.GetReferencePosition();

   /// Return distance between the sought and the found point in physical space,
   /// for each point found by FindPoints.
   Vector dist = finder.GetDist();

   MFEM_VERIFY(dist.Size() == np, "");
   MFEM_VERIFY(refcrd.Size() == np * dim, "");
   MFEM_VERIFY(elems.Size() == np, "");
   MFEM_VERIFY(codes.Size() == np, "");

   bool allfound = true;
   for (auto code : codes)
      if (code == 2) { allfound = false; }

   MFEM_VERIFY(allfound, "A point was not found");

   // reorder data so that the procs are in ascending order
   // sort procs and save the permutation
   std::vector<unsigned int> procs_index(np);
   std::iota(procs_index.begin(),procs_index.end(),0); //Initializing
   sort( procs_index.begin(),procs_index.end(), [&](int i,int j){return procs[i]<procs[j];} );

   // map to sorted
   Array<unsigned int> procs_sorted(np);
   Array<unsigned int> elems_sorted(np);
   Vector xyz_sorted(np*dim);
   Vector refcrd_sorted(np*dim);
   Array<int> s_conn_sorted(np);
   for (int i = 0; i<np; i++)
   {
      int j = procs_index[i];
      procs_sorted[i] = procs[j];
      elems_sorted[i] = elems[j];
      s_conn_sorted[i] = s_conn[j];
      for (int d = 0; d<dim; d++)
      {
         xyz_sorted(i+d*np) = xyz(j+d*np);
         refcrd_sorted(i*dim+d) = refcrd(j*dim+d);
      }
   }

   Array<unsigned int> elems_recv, proc_recv;
   xyz = xyz_sorted;
   s_conn = s_conn_sorted;
   Vector ref_recv;
   Vector xyz_recv;

   MPICommunicator mycomm(MPI_COMM_WORLD, procs_sorted);
   mycomm.Communicate(xyz_sorted,xyz_recv,3,mfem::Ordering::byNODES);
   mycomm.Communicate(elems_sorted,elems_recv,1,mfem::Ordering::byVDIM);
   mycomm.Communicate(refcrd_sorted,ref_recv,3,mfem::Ordering::byVDIM);


   proc_recv = mycomm.GetOriginProcs();

   int np_loc = elems_recv.Size();
   Array<int> conn_loc(np_loc*4);
   Vector xi_send(np_loc*(dim-1));
   for (int i=0; i<np_loc; ++i)
   {
      int refFace, refNormal, refNormalSide;
      bool is_interior = -1;

      Vector normal = GetNormalVector(mesh, elems_recv[i],
                                      ref_recv.GetData() + (i*dim),
                                      refFace, refNormal, is_interior);

      // continue;
      int phyFace;
      if (is_interior)
      {
         phyFace = -1; // the id of the face that has the closest point
         FindSurfaceToProject(mesh, elems_recv[i], phyFace); // seems that this works

         Array<int> cbdrVert;
         mesh.GetFaceVertices(phyFace, cbdrVert);
         Vector xs(dim);
         xs[0] = xyz_recv[i + 0*np_loc];
         xs[1] = xyz_recv[i + 1*np_loc];
         xs[2] = xyz_recv[i + 2*np_loc];

         Vector xi_tmp(dim-1);
         // get nodes!

         GridFunction *nodes = mesh.GetNodes();
         DenseMatrix coords(4,3);
         for (int i=0; i<4; i++)
         {
            for (int j=0; j<3; j++)
            {
               coords(i,j) = (*nodes)[cbdrVert[i]*3+j];
            }
         }
         SlaveToMaster(coords, xs, xi_tmp);

         for (int j=0; j<dim-1; ++j)
         {
            xi_send[i*(dim-1)+j] = xi_tmp[j];
         }
         // now get get the projection to the surface
      }
      else
      {
         Vector faceRefCrd(dim-1);
         {
            int fd = 0;
            for (int j=0; j<dim; ++j)
            {
               if (j == refNormal)
               {
                  refNormalSide = (ref_recv[(i*dim) + j] > 0.5);
               }
               else
               {
                  faceRefCrd[fd] = ref_recv[(i*dim) + j];
                  fd++;
               }
            }
            MFEM_VERIFY(fd == dim-1, "");
         }

         for (int j=0; j<dim-1; ++j)
         {
            xi_send[i*(dim-1)+j] = faceRefCrd[j]*2.0 - 1.0;
         }
      }
      // Get the element face
      Array<int> faces;
      Array<int> ori;
      int face;

      if (is_interior)
      {
         face = phyFace;
      }
      else
      {
         mesh.GetElementFaces(elems_recv[i], faces, ori);
         face = faces[refFace];
      }

      Array<int> faceVert;
      mesh.GetFaceVertices(face, faceVert);

      for (int p=0; p<4; p++)
      {
         conn_loc[4*i+p] = faceVert[p];
      }
   }

   if (0) // for debugging
   {
      int sz = xi_send.Size()/2;

      for (int i = 0; i<sz; i++)
      {
         mfem::out << "("<<xi_send[i*(dim-1)]<<","<<xi_send[i*(dim-1)+1]<<"): -> ";
         for (int j = 0; j<4; j++)
         {
            double * vc = mesh.GetVertex(conn_loc[4*i+j]);
            if (j<3)
            {
               mfem::out << "("<<vc[0]<<","<<vc[1]<<","<<vc[2]<<"), ";
            }
            else
            {
               mfem::out << "("<<vc[0]<<","<<vc[1]<<","<<vc[2]<<") \n " << endl;
            }
         }
      }
   }

   int sz = xi_send.Size()/2;
   DenseMatrix coordsm(sz*4, dim);
   for (int i = 0; i<sz; i++)
   {
      for (int j = 0; j<4; j++)
      {
         for (int k=0; k<dim; k++)
         {
            coordsm(i*4+j,k) = mesh.GetVertex(conn_loc[i*4+j])[k]+x1[dim*conn_loc[i*4+j]+k];
         }
      }
   }

   // pass global indices for conn_loc
   for (int i = 0; i<conn_loc.Size(); i++)
   {
      conn_loc[i] = gvert[conn_loc[i]];
   }

   mycomm.UpdateDestinationProcs();
   mycomm.Communicate(xi_send,xi,2,mfem::Ordering::byVDIM);
   mycomm.Communicate(conn_loc,conn,4,mfem::Ordering::byVDIM);
   mycomm.Communicate(coordsm,coords,4,mfem::Ordering::byVDIM);
}


int main(int argc, char *argv[])
{
   Mpi::Init();
   int num_procs = Mpi::WorldSize();
   int myid = Mpi::WorldRank();
   Hypre::Init();
   // 1. Parse command-line options.
   const char *mesh_file1 = "meshes/block1.mesh";
   const char *mesh_file2 = "meshes/block2.mesh";

   Array<int> attr;
   Array<int> m_attr;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file1, "-m1", "--mesh1",
                  "First mesh file to use.");
   args.AddOption(&mesh_file2, "-m2", "--mesh2",
                  "Second mesh file to use.");
   args.AddOption(&attr, "-at", "--attributes-surf",
                  "Attributes of boundary faces on contact surface for mesh 2.");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }

   Mesh mesh1(mesh_file1, 1, 1);
   Mesh mesh2(mesh_file2, 1, 1);

   const int dim = mesh1.Dimension();
   MFEM_VERIFY(dim == mesh2.Dimension(), "");

   // boundary attribute 2 is the potential contact surface of nodes
   attr.Append(3);
   // boundary attribute 2 is the potential contact surface for master surface
   m_attr.Append(3);

   ParMesh pmesh1(MPI_COMM_WORLD, mesh1); mesh1.Clear();
   ParMesh pmesh2(MPI_COMM_WORLD, mesh2); mesh2.Clear();

   char vishost[] = "localhost";
   int  visport   = 19916;

   socketstream mesh1_sock(vishost, visport);
   mesh1_sock << "parallel " << num_procs << " " << myid << "\n";
   mesh1_sock.precision(8);
   mesh1_sock << "mesh\n" << pmesh1 << flush;

   socketstream mesh2_sock(vishost, visport);
   mesh2_sock << "parallel " << num_procs << " " << myid << "\n";
   mesh2_sock.precision(8);
   mesh2_sock << "mesh\n" << pmesh2 << flush;

   FiniteElementCollection *fec1;
   ParFiniteElementSpace *fespace1;
   fec1 = new H1_FECollection(1, dim);
   fespace1 = new ParFiniteElementSpace(&pmesh1, fec1, dim, Ordering::byVDIM);
   HYPRE_BigInt size1 = fespace1->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of finite element unknowns for mesh1: " << size1 << endl;
   }
   pmesh1.SetNodalFESpace(fespace1);


   GridFunction nodes0 =
      *pmesh1.GetNodes(); // undeformed mesh1 nodal grid function
   GridFunction *nodes1 = pmesh1.GetNodes();

   FiniteElementCollection *fec2 = new H1_FECollection(1, dim);
   ParFiniteElementSpace *fespace2 = new ParFiniteElementSpace(&pmesh2, fec2, dim,
                                                               Ordering::byVDIM);
   HYPRE_BigInt size2 = fespace2->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of finite element unknowns for mesh2: " << size2 << endl;
   }

   // degrees of freedom of both meshes
   int ndof_1 = fespace1->GetTrueVSize();
   int ndof_2 = fespace2->GetTrueVSize();
   int ndofs = ndof_1 + ndof_2;
   int gndof_1 = fespace1->GlobalTrueVSize();
   int gndof_2 = fespace2->GlobalTrueVSize();
   int gndofs = gndof_1 + gndof_2;

   // find the total number of vertices owned
   ParFiniteElementSpace *vertexfes1 = new ParFiniteElementSpace(&pmesh1, fec1);
   ParFiniteElementSpace *vertexfes2 = new ParFiniteElementSpace(&pmesh2, fec2);
   int gnnd_1 = vertexfes1->GlobalTrueVSize();
   int gnnd_2 = vertexfes2->GlobalTrueVSize();

   int nnd_1 = vertexfes1->GetTrueVSize();
   int nnd_2 = vertexfes2->GetTrueVSize();


   int nnd = nnd_1 + nnd_2;
   int gnnd = gnnd_1 + gnnd_2;

   Array<int> ess_tdof_list1, ess_bdr1(pmesh1.bdr_attributes.Max());
   ess_bdr1 = 0;

   Array<int> ess_tdof_list2, ess_bdr2(pmesh2.bdr_attributes.Max());
   ess_bdr2 = 0;

   // Define the displacement vector x as a finite element grid function
   // corresponding to fespace. GridFunction is a derived class of Vector.
   ParGridFunction x1(fespace1); x1 = 0.0;
   ParGridFunction x2(fespace2); x2 = 0.0;

   // Generate force
   ParLinearForm *b1 = new ParLinearForm(fespace1);
   b1->Assemble();

   ParLinearForm *b2 = new ParLinearForm(fespace2);
   b2->Assemble();

   Vector lambda1(pmesh1.attributes.Max()); lambda1 = 57.6923076923;
   PWConstCoefficient lambda1_func(lambda1);
   Vector mu1(pmesh1.attributes.Max()); mu1 = 38.4615384615;
   PWConstCoefficient mu1_func(mu1);

   ParBilinearForm *a1 = new ParBilinearForm(fespace1);
   a1->AddDomainIntegrator(new ElasticityIntegrator(lambda1_func,mu1_func));
   a1->Assemble();

   Vector lambda2(pmesh2.attributes.Max()); lambda2 = 57.6923076923;
   PWConstCoefficient lambda2_func(lambda2);
   Vector mu2(pmesh2.attributes.Max());  mu2 = 38.4615384615;
   PWConstCoefficient mu2_func(mu2);

   ParBilinearForm *a2 = new ParBilinearForm(fespace2);
   a2->AddDomainIntegrator(new ElasticityIntegrator(lambda2_func,mu2_func));
   a2->Assemble();

   HypreParMatrix A1;
   Vector B1, X1;
   a1->FormLinearSystem(ess_tdof_list1, x1, *b1, A1, X1, B1);

   HypreParMatrix A2;
   Vector B2, X2;
   a2->FormLinearSystem(ess_tdof_list2, x2, *b2, A2, X2, B2);

   // Combine elasticity operator for two meshes into one.
   // Block Matrix
   Array2D<HypreParMatrix *> blkA(2,2);
   blkA(0,0) = &A1;
   blkA(1,1) = &A2;

   HypreParMatrix * K = HypreParMatrixFromBlocks(blkA);

   // Construct node to segment contact constraint.
   attr.Sort();
   // cout << "Boundary attributes for contact surface faces in mesh 2" << endl;
   // for (auto a : attr)  cout << a << endl;

   // unique numbering of vertices;
   Array<int> vertices1(pmesh1.GetNV());
   Array<int> vertices2(pmesh2.GetNV());

   for (int i = 0; i<pmesh1.GetNV(); i++)
   {
      vertices1[i] = i;
   }
   pmesh1.GetGlobalVertexIndices(vertices1);

   for (int i = 0; i<pmesh2.GetNV(); i++)
   {
      vertices2[i] = i;
   }
   pmesh2.GetGlobalVertexIndices(vertices2);

   // master mesh 1
   int voffset1 = vertexfes1->GetMyTDofOffset();
   int voffset2 = vertexfes2->GetMyTDofOffset();
   int voffset = voffset1 + voffset2;

   std::vector<int> vertex1_offsets;
   ComputeTdofOffsets(vertexfes1->GetComm(),voffset1,vertex1_offsets);
   std::vector<int> vertex2_offsets;
   ComputeTdofOffsets(vertexfes2->GetComm(),voffset2, vertex2_offsets);
   std::vector<int> vertex_offsets;
   ComputeTdofOffsets(vertexfes2->GetComm(),voffset, vertex_offsets);
   Array<int> globalvertices1(pmesh1.GetNV());
   Array<int> globalvertices2(pmesh2.GetNV());
   for (int i = 0; i<pmesh1.GetNV(); i++)
   {
      int rank = get_rank(vertices1[i],vertex1_offsets);
      globalvertices1[i] = vertices1[i] + vertex2_offsets[rank];
   }

   std::vector<int> vertex1_tdoffs;
   ComputeTdofOffsets(vertexfes1->GetComm(),nnd_1, vertex1_tdoffs);

   for (int i = 0; i<pmesh2.GetNV(); i++)
   {
      int rank = get_rank(vertices2[i],vertex2_offsets);
      globalvertices2[i] = vertices2[i] + vertex1_offsets[rank] + vertex1_tdoffs[rank];
   }

   std::set<int> bdryVerts2;
   for (int b=0; b<pmesh2.GetNBE(); ++b)
   {
      if (attr.FindSorted(pmesh2.GetBdrAttribute(b)) >= 0)
      {
         Array<int> vert;
         pmesh2.GetBdrElementVertices(b, vert);
         for (auto v : vert)
         {
            // skip if the processor does not own the vertex
            if (myid != get_rank(globalvertices2[v],vertex_offsets)) { continue; }
            bdryVerts2.insert(v);
         }
      }
   }

   int npoints = bdryVerts2.size();

   Array<int> s_conn(npoints); // connectivity of the second/slave mesh
   Vector xyz(dim * npoints);
   xyz = 0.0;

   // cout << "Boundary vertices for contact surface vertices in mesh 2" << endl;

   // construct the nodal coordinates on mesh2 to be projected, including displacement
   int count = 0;
   for (auto v : bdryVerts2)
   {
      for (int i=0; i<dim; ++i)
      {
         xyz[count + (i * npoints)] = pmesh2.GetVertex(v)[i] + x2[v*dim+i];
      }
      s_conn[count] = globalvertices2[v];
      count++;
   }
   MFEM_VERIFY(count == npoints, "");


   // gap function
   Vector g(npoints*dim);
   g = -1.0;
   // segment reference coordinates of the closest point
   Vector m_xi(npoints*(dim-1));
   m_xi = -1.0;

   Array<int> m_conn(npoints*4); // only works for linear elements that have 4 vertices!
   DenseMatrix coordsm(npoints*4, dim);

   // adding displacement to mesh1 using a fixed grid function from mesh1
   x1 = 1e-4; // x1 order: [xyz xyz... xyz]
   add(nodes0, x1, *nodes1);

   // s_conn is reordered matching m_conn ordering
   // This can be simplified later
   // Vector xyz_recv;
   // Array<int> s_conn_recv;
   // FindPointsInMesh(pmesh1, globalvertices1, xyz, s_conn, m_conn, xyz_recv, s_conn_recv, m_xi ,coordsm);
   // s_conn = s_conn_recv;
   // xyz = xyz_recv;


   // note that s_conn and xyz are reordered.
   FindPointsInMesh(pmesh1, globalvertices1, s_conn, x1, xyz, m_conn, m_xi ,coordsm);

   // decode and print
   if (0) // for debugging
   {
      int sz = m_xi.Size()/2;
      for (int i = 0; i<sz; i++)
      {
         mfem::out << "("<<xyz[i+0*sz]<<","<<xyz[i+1*sz]<<","<<xyz[i+2*sz]<<"): -> ";
         mfem::out << "("<<m_xi[i*(dim-1)]<<","<<m_xi[i*(dim-1)+1]<<"): -> ";
         for (int j = 0; j<4; j++)
         {
            if (j<3)
            {
               mfem::out << "("<<coordsm(i*4+j,0)<<","<<coordsm(i*4+j,1)<<","<<coordsm(i*4+j,
                                                                                       2)<<"), ";
            }
            else
            {
               mfem::out << "("<<coordsm(i*4+j,0)<<","<<coordsm(i*4+j,1)<<","<<coordsm(i*4+j,
                                                                                       2)<<") \n " << endl;
            }
         }
      }

      MPI_Barrier(MPI_COMM_WORLD);
      // debug m_conn
      for (int i = 0; i< pmesh1.GetNV(); i++)
      {
         int gv = globalvertices1[i];
         if (myid != get_rank(gv,vertex1_offsets)) continue;
         double *vcoords = pmesh1.GetVertex(i); 
         mfem::out << "vertex1: " << gv << " = ("<<vcoords[0] <<","<<vcoords[1]<<","<<vcoords[2]<<")" << endl;
      }
      MPI_Barrier(MPI_COMM_WORLD);
      for (int i = 0; i< pmesh2.GetNV(); i++)
      {
         int gv = globalvertices2[i];
         int rank = get_rank(gv,vertex_offsets);
         if (myid != rank) continue;
         double *vcoords = pmesh2.GetVertex(i); 
         mfem::out << "vertex2: " << gv << " = ("<<vcoords[0] <<","<<vcoords[1]<<","<<vcoords[2]<<")" << endl;
      }
   }
   
   Vector xs(dim*npoints);
   xs = 0.0;
   for (int i=0; i<npoints; i++)
   {
      for (int j=0; j<dim; j++)
      {
         xs[i*dim+j] = xyz[i + (j*npoints)];
      }
   }

   SparseMatrix M(gnnd,gndofs);

   Array<int> npts(num_procs);
   MPI_Allgather(&npoints,1,MPI_INT,&npts[0],1,MPI_INT,MPI_COMM_WORLD);
   npts.PartialSum(); npts.Prepend(0);

   int gnpts = npts[num_procs];
   Array<SparseMatrix *> dM(gnpts);
   for (int i = 0; i<gnpts; i++)
   {
      if (i >= npts[myid] && i< npts[myid+1])
      {
         dM[i] = new SparseMatrix(gndofs,gndofs);
      }
      else
      {
         dM[i] = nullptr;
      }
   }

   Assemble_Contact(gnnd, xs, m_xi, coordsm, s_conn, m_conn, g, M, dM);
   // --------------------------------------------------------------------
   // Redistribute the M matrix
   // --------------------------------------------------------------------
   MPICommunicator Mcomm(K->GetComm(),voffset,gnnd);
   SparseMatrix localM(nnd,K->GetGlobalNumCols());
   Mcomm.Communicate(M,localM);
   // --------------------------------------------------------------------

   // --------------------------------------------------------------------
   // Redistribute the dM_i matrices
   // --------------------------------------------------------------------
   MPICommunicator dmcomm(K->GetComm(), K->RowPart()[0], gndofs);
   Array<SparseMatrix*> localdMs(gnpts);
   for (int k = 0; k<gnpts; k++)
   {
      localdMs[k] = new SparseMatrix(ndofs,gndofs); 
   }
   dmcomm.Communicate(dM,localdMs);
   // --------------------------------------------------------------------



   SparseMatrix localDM = *Add(localdMs);

   // Assume this is true
   MFEM_VERIFY(HYPRE_AssumedPartitionCheck(), "Hypre_AssumedPartitionCheck is False");

   localDM.Threshold(1e-15);
   localM.Threshold(1e-15);

   // Construct M row and col starts to construct HypreParMatrix
   int Mrows[2]; Mrows[0] = vertex_offsets[myid]; Mrows[1] = vertex_offsets[myid]+nnd;
   int Mcols[2]; Mcols[0] = K->ColPart()[0]; Mcols[1] = K->ColPart()[1]; 
   HypreParMatrix hypreM(K->GetComm(),nnd,gnnd,gndofs,
                         localM.GetI(), localM.GetJ(),localM.GetData(),
                         Mrows,Mcols);

   int DMrows[2]; DMrows[0] = K->RowPart()[0]; DMrows[1] = K->RowPart()[1];
   int DMcols[2]; DMcols[0] = K->ColPart()[0]; DMcols[1] = K->ColPart()[1]; 

   HypreParMatrix hypreDM(K->GetComm(),ndofs,gndofs,gndofs,
                          localDM.GetI(), localDM.GetJ(),localDM.GetData(),
                          DMrows,DMcols);  

   HypreParMatrix * mat = ParAdd(K,&hypreDM);

   VectorFunctionCoefficient cf1(dim,rhs_func1);
   VectorFunctionCoefficient cf2(dim,rhs_func2);

   ParGridFunction gf1(fespace1); gf1.ProjectCoefficient(cf1);
   ParGridFunction gf2(fespace2); gf2.ProjectCoefficient(cf2);

   Vector rhs1(fespace1->GetTrueVSize()), rhs2(fespace2->GetTrueVSize());
   gf1.ParallelProject(rhs1);
   gf2.ParallelProject(rhs2);

   Vector X(rhs1.Size()+rhs2.Size());
   X.SetVector(rhs1,0);
   X.SetVector(rhs2,rhs1.Size());

   Vector YDM(hypreDM.Height());
   Vector YM(hypreM.Height());

   hypreM.Mult(X,YM);
   hypreDM.Mult(X,YDM);
   double ydmnorm = InnerProduct(MPI_COMM_WORLD,YDM,YDM);
   double ymnorm = InnerProduct(MPI_COMM_WORLD,YM,YM);

   mfem::out << "ymnorm = " << ymnorm << endl;
   mfem::out << "ydmnorm = " << ydmnorm << endl;

   // --------------------------------------------------------------------
   // std::set<int> dirbdryv2;
   // for (int b=0; b<mesh2.GetNBE(); ++b)
   // {
   //    if (mesh2.GetBdrAttribute(b) == 1)
   //    {
   //       Array<int> vert;
   //       mesh2.GetBdrElementVertices(b, vert);
   //       for (auto v : vert)
   //       {
   //          dirbdryv2.insert(v);
   //       }
   //    }
   // }
   // std::set<int> dirbdryv1;
   // for (int b=0; b<mesh1.GetNBE(); ++b)
   // {
   //    if (mesh1.GetBdrAttribute(b) == 1)
   //    {
   //       Array<int> vert;
   //       mesh1.GetBdrElementVertices(b, vert);
   //       for (auto v : vert)
   //       {
   //          dirbdryv1.insert(v);
   //       }
   //    }
   // }

   // Array<int> Dirichlet_dof;
   // Array<double> Dirichlet_val;

   // for (auto v : dirbdryv2)
   // {
   //    for (int i=0; i<dim; ++i)
   //    {
   //       Dirichlet_dof.Append(v*dim + i + ndof_1);
   //       Dirichlet_val.Append(0.);
   //    }
   // }
   // double delta = 0.1;
   // for (auto v : dirbdryv1)
   // {
   //    Dirichlet_dof.Append(v*dim + 0);
   //    Dirichlet_val.Append(delta);
   //    Dirichlet_dof.Append(v*dim + 1);
   //    Dirichlet_val.Append(0.);
   //    Dirichlet_dof.Append(v*dim + 2);
   //    Dirichlet_val.Append(0.);
   // }
   return 0;
}
