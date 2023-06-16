//                                Contact example
//
// Compile with: make contact
//
// Sample runs:  ./contact -m1 block1.mesh -m2 block2.mesh -at "5 6 7 8"
// Sample runs:  ./contact -m1 block1_d.mesh -m2 block2_d.mesh -at "5 6 7 8"

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include "nodepair.hpp"

using namespace std;
using namespace mfem;

void PrintElementVertices(Mesh * mesh, int elem,  int printid)
{
   int myid = Mpi::WorldRank();
   Array<int> vertices;
   if (myid == printid)
   {
      mfem::out << "myid = " << myid <<":   " <<  "elem: " << elem << ". Vertices = \n" ;
      mesh->GetElementVertices(elem,vertices);
      for (int i = 0; i<vertices.Size(); i++)
      {
         double * coords = mesh->GetVertex(vertices[i]);
         mfem::out << "(" << coords[0] << ", " << coords[1] << ", " << coords[2] << ")" << endl;
      }
      mfem::out << endl;
   }
}

void PrintFaceVertices(Mesh * mesh, int face,  int printid)
{
   int myid = Mpi::WorldRank();
   Array<int> vertices;
   if (myid == printid)
   {
      mfem::out << "myid = " << myid <<":   " <<  "face: " << face << ". Vertices = \n" ;
      mesh->GetFaceVertices(face,vertices);
      for (int i = 0; i<vertices.Size(); i++)
      {
         double *coords = mesh->GetVertex(vertices[i]);
         mfem::out << "(" << coords[0] << ", " << coords[1] << ", " << coords[2] << ")" << endl;
      }
      mfem::out << endl;
   }
}

template <class T>
void PrintArray(const Array<T> & a, const char *aname,  int printid)
{
   int myid = Mpi::WorldRank();
   if (myid == printid)
   {
      int sz = a.Size();
      mfem::out << "myid = " << myid <<":   " << aname << " = " ;
      for (int i = 0; i<sz; i++)
      {
         mfem::out << a[i] << "  ";
      }
      mfem::out << endl;
   }
}

void PrintSet(const std::set<int> & a, const char *aname,  int printid)
{
   int myid = Mpi::WorldRank();
   if (myid == printid)
   {
      mfem::out << "myid = " << myid <<":   " << aname << " = " ;
      for (std::set<int>::iterator it = a.begin(); it!= a.end(); it++)
      {
         mfem::out << *it << "  ";
      }
      mfem::out << endl;
   }
}

void PrintVector(const Vector & a, const char *aname,  int printid)
{
   int myid = Mpi::WorldRank();
   if (myid == printid)
   {
      int sz = a.Size();
      mfem::out << "myid = " << myid <<":   " << aname << " = " ;
      for (int i = 0; i<sz; i++)
      {
         mfem::out << a[i] << "  ";
      }
      mfem::out << endl;
   }
}

void FindSurfaceToProject(Mesh& mesh, const int elem, int& cbdrface)
{
   Array<int> attr;
   attr.Append(2);
   Array<int> faces;
   Array<int> ori;
   std::vector<Array<int> > facesVertices;
   std::vector<int > faceid;
   mesh.GetElementFaces(elem, faces, ori);
   int face = -1;
   for(int i=0; i<faces.Size(); i++)
   {
      face = faces[i];
      Array<int> faceVert;
      if(!mesh.FaceIsInterior(face)) // if on the boundary 
      {
         mesh.GetFaceVertices(face, faceVert);
	      faceVert.Sort();
	      facesVertices.push_back(faceVert);
	      faceid.push_back(face);
      }
   }
   int bdrface = facesVertices.size(); 

   Array<int> bdryFaces;  
   // This shoulnd't need to be rebuilt
   std::vector<Array<int> > bdryVerts;  
   for (int b=0; b<mesh.GetNBE(); ++b)
   {
      if (attr.FindSorted(mesh.GetBdrAttribute(b)) >= 0)  // found the contact surface
      {
         bdryFaces.Append(b);
         Array<int> vert;
         mesh.GetBdrElementVertices(b, vert);
         vert.Sort();
         bdryVerts.push_back(vert); 
      }
   }

   int bdrvert = bdryVerts.size();
   cbdrface = -1;  // the face number of the contact surface element
   int count_cbdrface = 0;  // the number of matching surfaces, used for checks

   for (int i=0; i<bdrface; i++)
   {
       for (int j=0; j<bdrvert; j++) 
       {
	   if(facesVertices[i] == bdryVerts[j])
	   {
	       cbdrface = faceid[i];
	       count_cbdrface += 1; 
	   }
       }
   }
   MFEM_VERIFY(count_cbdrface == 1,"projection surface not found");
   
};

Vector GetNormalVector(Mesh & mesh, const int elem, const double *ref,
                       int & refFace, int & refNormal, bool & interior)
{

   ElementTransformation *trans = mesh.GetElementTransformation(elem);
   const int dim = mesh.Dimension();
   const int spaceDim = trans->GetSpaceDim();

   MFEM_VERIFY(spaceDim == 3, "");

   Vector n(spaceDim);

   IntegrationPoint ip;
   ip.Set(ref, dim);

   trans->SetIntPoint(&ip);
   //CalcOrtho(trans->Jacobian(), n);  // Works only for face transformations
   const DenseMatrix jac = trans->Jacobian();

   int dimNormal = -1;
   int normalSide = -1;

   const double tol = 1.0e-8;
   for (int i=0; i<dim; ++i)
   {
      const double d0 = std::abs(ref[i]);
      const double d1 = std::abs(ref[i] - 1.0);

      const double d = std::min(d0, d1);
      // TODO: this works only for hexahedral meshes!

      if (d < tol)
      {
         MFEM_VERIFY(dimNormal == -1, "");
         dimNormal = i;

         if (d0 < tol)
         {
            normalSide = 0;
         }
         else
         {
            normalSide = 1;
         }
      }
   }
   // closest point on the boundary
   if(dimNormal < 0 || normalSide < 0) // node is inside the element
   {
       interior = 1;
       Vector n(3);
       n = 0.0;
       return n;  
   }
   
   MFEM_VERIFY(dimNormal >= 0 && normalSide >= 0, "");
   refNormal = dimNormal;

   MFEM_VERIFY(dim == 3, "");

   {
      // Find the reference face
      if (dimNormal == 0)
      {
         refFace = (normalSide == 1) ? 2 : 4;
      }
      else if (dimNormal == 1)
      {
         refFace = (normalSide == 1) ? 3 : 1;
      }
      else
      {
         refFace = (normalSide == 1) ? 5 : 0;
      }
   }

   std::vector<Vector> tang(2);

   int tangDir[2] = {-1, -1};
   {
      int t = 0;
      for (int i=0; i<dim; ++i)
      {
         if (i != dimNormal)
         {
            tangDir[t] = i;
            t++;
         }
      }

      MFEM_VERIFY(t == 2, "");
   }

   for (int i=0; i<2; ++i)
   {
      tang[i].SetSize(3);

      Vector tangRef(3);
      tangRef = 0.0;
      tangRef[tangDir[i]] = 1.0;

      jac.Mult(tangRef, tang[i]);
   }

   Vector c(3);  // Cross product

   c[0] = (tang[0][1] * tang[1][2]) - (tang[0][2] * tang[1][1]);
   c[1] = (tang[0][2] * tang[1][0]) - (tang[0][0] * tang[1][2]);
   c[2] = (tang[0][0] * tang[1][1]) - (tang[0][1] * tang[1][0]);

   c /= c.Norml2();

   Vector nref(3);
   nref = 0.0;
   nref[dimNormal] = 1.0;

   Vector ndir(3);
   jac.Mult(nref, ndir);

   ndir /= ndir.Norml2();

   const double dp = ndir * c;

   // TODO: eliminate c?
   n = c;
   if (dp < 0.0)
   {
      n *= -1.0;
   }
   interior = 0;
   return n;
}

// WARNING: global variable, just for this little example.
std::array<std::array<int, 3>, 8> HEX_VERT =
{
   {  {0,0,0},
      {1,0,0},
      {1,1,0},
      {0,1,0},
      {0,0,1},
      {1,0,1},
      {1,1,1},
      {0,1,1}
   }
};

int GetHexVertex(int cdim, int c, int fa, int fb, Vector & refCrd)
{
   int ref[3];
   ref[cdim] = c;
   ref[cdim == 0 ? 1 : 0] = fa;
   ref[cdim == 2 ? 1 : 2] = fb;

   for (int i=0; i<3; ++i) { refCrd[i] = ref[i]; }

   int refv = -1;

   for (int i=0; i<8; ++i)
   {
      bool match = true;
      for (int j=0; j<3; ++j)
      {
         if (ref[j] != HEX_VERT[i][j]) { match = false; }
      }

      if (match) { refv = i; }
   }

   MFEM_VERIFY(refv >= 0, "");

   return refv;
}

// Coordinates in xyz are assumed to be ordered as [X, Y, Z]
// where X is the list of x-coordinates for all points and so on.
// conn: connectivity of the target surface elements
// xi: surface reference cooridnates for the cloest point, involves a linear transformation from [0,1] to [-1,1]
void FindPointsInMesh(Mesh & mesh, Vector const& xyz, Array<int>& conn, Vector& xi)
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

   // extract information
   int myid = Mpi::WorldRank();
   GSLIBCommunicator gslcomm(MPI_COMM_WORLD);

   Array<unsigned int> index_recv, elems_recv, proc_recv;
   Vector ref_recv;
   
   // PrintArray(procs,"procs", 0);
   PrintArray(procs,"procs", 1);

   Vector xyz_recv;
   gslcomm.SendData(dim,procs,elems,refcrd,xyz, proc_recv,index_recv,elems_recv,ref_recv, xyz_recv);


   PrintVector(refcrd, "ref send", 0);
   PrintVector(refcrd, "ref send", 1);

   PrintVector(ref_recv, "ref recv", 0);
   PrintVector(ref_recv, "ref recv", 1);

   // PrintArray(elems, "elems", 0);
   // PrintArray(elems, "elems", 1);
   // PrintArray(elems_recv, "elems_recv", 0);
   // PrintArray(elems_recv, "elems_recv", 1);
   // PrintArray(elems_recv, "elements", 1);
   PrintVector(xyz, "xyz ", 0);
   PrintVector(xyz, "xyz ", 1);
   PrintVector(xyz_recv, "xyz_recv ", 0);
   PrintVector(xyz_recv, "xyz_recv ", 1);
   mfem::out << "np = " << np << endl;
   int np_loc = elems_recv.Size();
   for (int i=0; i<elems_recv.Size(); ++i)
   {
      int refFace, refNormal, refNormalSide;
      bool is_interior = -1;
      
      Vector normal = GetNormalVector(mesh, elems_recv[i], ref_recv.GetData() + (i*dim),
                                      refFace, refNormal, is_interior);

      // PrintElementVertices(&mesh,elems_recv[i],1);                                      
      int phyFace;
      if(is_interior)
      {
         phyFace = -1; // the id of the face that has the closest point
         FindSurfaceToProject(mesh, elems_recv[i], phyFace); // seems that this works

         // PrintFaceVertices(&mesh,phyFace,1);
         Array<int> cbdrVert;
         mesh.GetFaceVertices(phyFace, cbdrVert);
	      Vector xs(dim);
         // xs[0] = xyz_recv[i + 0*np_loc];
         // xs[1] = xyz_recv[i + 1*np_loc];
         // xs[2] = xyz_recv[i + 2*np_loc];
  


         PrintVector(xs,"xs = ", 0);
         PrintVector(xs,"xs = ", 1);

	      Vector xi_tmp(dim-1);
         // // get nodes!

         GridFunction *nodes = mesh.GetNodes();
         DenseMatrix coords(4,3); 
         for (int i=0; i<4; i++)
	      {
	         for (int j=0; j<3; j++)
	         {
               coords(i,j) = (*nodes)[cbdrVert[i]*3+j];
	         }
	      }
	      // SlaveToMaster(coords, xs, xi_tmp);

         // for (int j=0; j<dim-1; ++j)
         // {
	      //    xi[i*(dim-1)+j] = xi_tmp[j];
         // }
	//       // now get get the projection to the surface 
      }
      else
      {
   //       Vector faceRefCrd(dim-1);
   //       {
   //          int fd = 0;
   //          for (int j=0; j<dim; ++j)
   //          {
   //             if (j == refNormal)
   //             {
   //                refNormalSide = (refcrd[(i*dim) + j] > 0.5);
   //             }
   //             else
   //             {
   //                faceRefCrd[fd] = refcrd[(i*dim) + j];
   //                fd++;
   //             }
   //          }
   //          MFEM_VERIFY(fd == dim-1, "");
   //       }

   //       for (int j=0; j<dim-1; ++j)
   //       {
	//          xi[i*(dim-1)+j] = faceRefCrd[j]*2.0 - 1.0;
   //       }
      }
   //    // Get the element face
   //    Array<int> faces;
   //    Array<int> ori;
   //    int face;

   //    if(is_interior)
   //    {
   //       face = phyFace;
   //    }
   //    else
   //    {
   //       mesh.GetElementFaces(elems[i], faces, ori);
   //       face = faces[refFace];
   //    }

   //    Array<int> faceVert;
   //    mesh.GetFaceVertices(face, faceVert);

   //    for (int p=0; p<4; p++)
   //    {
   //       conn[4*i+p] = faceVert[p];
   //    }
   }
}

int main(int argc, char *argv[])
{
   Mpi::Init();
   int num_procs = Mpi::WorldSize();
   int myid = Mpi::WorldRank();
   Hypre::Init();
   // 1. Parse command-line options.
   const char *mesh_file1 = "block1.mesh";
   const char *mesh_file2 = "block2.mesh";

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
   args.PrintOptions(cout);

   Mesh mesh1(mesh_file1, 1, 1);
   Mesh mesh2(mesh_file2, 1, 1);

   const int dim = mesh1.Dimension();
   MFEM_VERIFY(dim == mesh2.Dimension(), "");

   // boundary attribute 2 is the potential contact surface of nodes
   attr.Append(2);
   // boundary attribute 2 is the potential contact surface for master surface 
   m_attr.Append(2);

   ParMesh pmesh1(MPI_COMM_WORLD, mesh1); mesh1.Clear();
   ParMesh pmesh2(MPI_COMM_WORLD, mesh2); mesh2.Clear();

   char vishost[] = "localhost";
   int  visport   = 19916;
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
   

   GridFunction nodes0 = *pmesh1.GetNodes(); // undeformed mesh1 nodal grid function
   GridFunction *nodes1 = pmesh1.GetNodes();

   FiniteElementCollection *fec2 = new H1_FECollection(1, dim);
   ParFiniteElementSpace *fespace2 = new ParFiniteElementSpace(&pmesh2, fec2, dim, Ordering::byVDIM);
   HYPRE_BigInt size2 = fespace2->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of finite element unknowns for mesh2: " << size2 << endl;
   }

   // degrees of freedom of both meshes
   int ndof_1 = fespace1->GetTrueVSize();
   int ndof_2 = fespace2->GetTrueVSize();
   int ndofs = ndof_1 + ndof_2;
   // number of nodes for each mesh
   int nnd_1 = pmesh1.GetNV();
   int nnd_2 = pmesh2.GetNV();
   int nnd = nnd_1 + nnd_2;

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

   pmesh2.ExchangeFaceNbrData();

   std::set<int> bdryVerts2;  
   for (int b=0; b<pmesh2.GetNBE(); ++b)
   {
      // mfem::Mesh::FaceInformation info = pmesh2.GetFaceInformation(pmesh2.GetBdrFace(b));
   
      if (attr.FindSorted(pmesh2.GetBdrAttribute(b)) >= 0)
      {
         Array<int> vert;
         pmesh2.GetBdrElementVertices(b, vert);
         for (auto v : vert)
         {
            bdryVerts2.insert(v);
         }
      }
   }

   PrintSet(bdryVerts2, "bdrVerts2", 0);
   PrintSet(bdryVerts2, "bdrVerts2", 1);

   int npoints = bdryVerts2.size();


   Array<int> s_conn(npoints); // connectivity of the second/slave mesh 
   Vector xyz(dim * npoints);
   xyz = 0.0;  

   cout << "Boundary vertices for contact surface vertices in mesh 2" << endl;

   // construct the nodal coordinates on mesh2 to be projected, including displacement
   int count = 0;
   for (auto v : bdryVerts2)
   {
      cout << v << ": " << pmesh2.GetVertex(v)[0] << ", "
           << pmesh2.GetVertex(v)[1] << ", "
           << pmesh2.GetVertex(v)[2] << endl;

      for (int i=0; i<dim; ++i)
      {
         xyz[count + (i * npoints)] = pmesh2.GetVertex(v)[i] + x2[v*dim+i]; 
      }
      
      s_conn[count] = v + nnd_1; // dof1 is the master
      count++;
   }

   MFEM_VERIFY(count == npoints, "");

   // gap function
   Vector g(npoints*dim);
   g = -1.0;
   // segment reference coordinates of the closest point
   Vector m_xi(npoints*(dim-1));
   m_xi = -1.0;
   Vector xs(dim*npoints);
   xs = 0.0;
   for (int i=0; i<npoints; i++)
   {
      for (int j=0; j<dim; j++)
      {
         xs[i*dim+j] = xyz[i + (j*npoints)];
      }
   }
   
   Array<int> m_conn(npoints*4); // only works for linear elements that have 4 vertices!
   DenseMatrix coordsm(npoints*4, dim);
 
   // adding displacement to mesh1 using a fixed grid function from mesh1
   x1 = 1e-4; // x1 order: [xyz xyz... xyz]
   add(nodes0, x1, *nodes1);
   
   FindPointsInMesh(pmesh1, xyz, m_conn, m_xi);
   return 0;

   for (int i=0; i<npoints; i++)
   {
      for (int j=0; j<4; j++)
      {
	      for (int k=0; k<dim; k++)
	      {
            coordsm(i*4+j,k) = mesh1.GetVertex(m_conn[i*4+j])[k]+x1[dim*m_conn[i*4+j]+k];     
	      }
      }
   }

   SparseMatrix M(nnd,ndofs);
   std::vector<SparseMatrix> dM(nnd, SparseMatrix(ndofs,ndofs));
   
   Assemble_Contact(nnd, npoints, ndofs, xs, m_xi, coordsm, 
		    s_conn, m_conn, g, M, dM);

   std::set<int> dirbdryv2;  
   for (int b=0; b<mesh2.GetNBE(); ++b)
   {
      if (mesh2.GetBdrAttribute(b) == 1)
      {
         Array<int> vert;
         mesh2.GetBdrElementVertices(b, vert);
         for (auto v : vert)
         {
            dirbdryv2.insert(v);
         }
      }
   }
   std::set<int> dirbdryv1;  
   for (int b=0; b<mesh1.GetNBE(); ++b)
   {
      if (mesh1.GetBdrAttribute(b) == 1)
      {
         Array<int> vert;
         mesh1.GetBdrElementVertices(b, vert);
         for (auto v : vert)
         {
            dirbdryv1.insert(v);
         }
      }
   }

   Array<int> Dirichlet_dof;    
   Array<double> Dirichlet_val;    
   
   for (auto v : dirbdryv2)
   {
      for (int i=0; i<dim; ++i)
      {
	      Dirichlet_dof.Append(v*dim + i + ndof_1);
	      Dirichlet_val.Append(0.);
      }
   }
   double delta = 0.1;
   for (auto v : dirbdryv1)
   {
      Dirichlet_dof.Append(v*dim + 0);
      Dirichlet_val.Append(delta);
      Dirichlet_dof.Append(v*dim + 1);
      Dirichlet_val.Append(0.);
      Dirichlet_dof.Append(v*dim + 2);
      Dirichlet_val.Append(0.);
   }
   return 0;
}
