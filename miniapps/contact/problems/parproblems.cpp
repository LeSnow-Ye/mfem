#include "parproblems.hpp"


ElasticityOperator::ElasticityOperator(ParMesh * pmesh_, Array<int> & ess_bdr_attr_, Array<int> & ess_bdr_attr_comp_,
                       const Vector & E, const Vector & nu, bool nonlinear_)
:pmesh(pmesh_), ess_bdr_attr(ess_bdr_attr_), ess_bdr_attr_comp(ess_bdr_attr_comp_), nonlinear(nonlinear_)
{
   comm = pmesh->GetComm();
   SetParameters(E,nu);
   Init();
} 

void ElasticityOperator::SetParameters(const Vector & E, const Vector & nu)
{
   int n = (pmesh->attributes.Size()) ?  pmesh->attributes.Max() : 0;
   MFEM_VERIFY(E.Size() == n, "Incorrect parameter size E");
   MFEM_VERIFY(nu.Size() == n, "Incorrect parameter size nu");
   c1.SetSize(n);
   c2.SetSize(n);
   if (nonlinear)
   {
      for (int i = 0; i<n; i++)
      {
         c1(i) = 0.5*E(i) / (1+nu(i));
         c2(i) = E(i)/(1-2*nu(i))/3;
      }
   }
   else
   {
      for (int i = 0; i<n; i++)
      {
         c1(i) = E(i) * nu(i) / ( (1+nu(i)) * (1-2*nu(i)) );
         c2(i) = 0.5 * E(i)/(1+nu(i));
      }
   }
   c1_cf.UpdateConstants(c1);
   c2_cf.UpdateConstants(c2);
}
  

void ElasticityOperator::Init()
{
   int dim = pmesh->Dimension();
   fec = new H1_FECollection(order,dim);
   fes = new ParFiniteElementSpace(pmesh,fec,dim,Ordering::byVDIM);
   ndofs = fes->GetVSize();
   ntdofs = fes->GetTrueVSize();
   gndofs = fes->GlobalTrueVSize();
   pmesh->SetNodalFESpace(fes);

   auto ref_func = [](const Vector & x, Vector & y) { y = x; };
   VectorFunctionCoefficient ref_cf(dim,ref_func);
   ParGridFunction xr(fes); xr.ProjectCoefficient(ref_cf);
   xr.GetTrueDofs(xref);
   SetEssentialBC();
   SetUpOperator();
}

void ElasticityOperator::SetEssentialBC()
{
   ess_tdof_list.SetSize(0);
   if (pmesh->bdr_attributes.Size())
   {
      ess_bdr.SetSize(pmesh->bdr_attributes.Max());
   }
   ess_bdr = 0; 
   Array<int> ess_tdof_list_temp;
   for (int i = 0; i < ess_bdr_attr.Size(); i++ )
   {
      ess_bdr[ess_bdr_attr[i]-1] = 1;
      fes->GetEssentialTrueDofs(ess_bdr,ess_tdof_list_temp,ess_bdr_attr_comp[i]);
      ess_tdof_list.Append(ess_tdof_list_temp);
      ess_bdr[ess_bdr_attr[i]-1] = 0;
   }
}

void ElasticityOperator::SetUpOperator()
{
   x.SetSpace(fes);  x = 0.0;
   b = new ParLinearForm(fes);
   if (nonlinear)
   {
      material_model = new NeoHookeanModel(c1_cf, c2_cf);
      op = new ParNonlinearForm(fes);
      dynamic_cast<ParNonlinearForm*>(op)->AddDomainIntegrator(new HyperelasticNLFIntegrator(material_model));
      dynamic_cast<ParNonlinearForm*>(op)->SetEssentialTrueDofs(ess_tdof_list);
   }
   else
   {
      op = new ParBilinearForm(fes);
      dynamic_cast<ParBilinearForm*>(op)->AddDomainIntegrator(new ElasticityIntegrator(c1_cf,c2_cf));
      K = new HypreParMatrix();
      dynamic_cast<ParBilinearForm*>(op)->Assemble();
      dynamic_cast<ParBilinearForm*>(op)->FormSystemMatrix(ess_tdof_list,*K);
   }
}

void ElasticityOperator::FormLinearSystem()
{
   if (!formsystem) 
   {
      formsystem = true;
      b->Assemble();
      B.SetSize(ntdofs);
      b->ParallelAssemble(B);
      B.SetSubVector(ess_tdof_list, 0.0);
      if (!nonlinear)
      {
         x.GetTrueDofs(X);
         dynamic_cast<ParBilinearForm*>(op)->EliminateVDofsInRHS(ess_tdof_list, X, B);
      }
   }
}

void ElasticityOperator::UpdateRHS()
{
   formsystem = false;
   delete b;
   b = new ParLinearForm(fes);
}

void ElasticityOperator::SetNeumanPressureData(ConstantCoefficient &f, Array<int> & bdr_marker)
{
   pressure_cf.constant = f.constant;
   b->AddBoundaryIntegrator(new VectorBoundaryFluxLFIntegrator(pressure_cf),bdr_marker);
}

void ElasticityOperator::SetDisplacementDirichletData(const Vector & delta, Array<int> essbdr)
{
   VectorConstantCoefficient delta_cf(delta);
   x.ProjectBdrCoefficient(delta_cf,essbdr);
} 

void ElasticityOperator::ResetDisplacementDirichletData() { x = 0.0; }

void ElasticityOperator::UpdateEssentialBC(Array<int> & ess_bdr_attr_, Array<int> & ess_bdr_attr_comp_)
{
   ess_bdr_attr = ess_bdr_attr_;
   ess_bdr_attr_comp = ess_bdr_attr_comp_;
   SetEssentialBC();
}

const real_t ElasticityOperator::GetEnergy(const Vector & u) const
{
   if (nonlinear)
   {
      real_t energy = 0.0;
      Vector tu(xref); tu += u;
      ParGridFunction u_gf(fes);
      u_gf.SetFromTrueDofs(tu);
      energy += dynamic_cast<ParNonlinearForm*>(op)->GetEnergy(u_gf);
      energy -= InnerProduct(comm, B, u);
      return energy;
   }
   else
   {
      Vector ku(K->Height());
      K->Mult(u,ku);
      return 0.5 * InnerProduct(comm,u, ku) - InnerProduct(comm,u, B);
   }
}

const void ElasticityOperator::GetGradient(const Vector & u, Vector & gradE) const
{
   if (nonlinear)
   {
      Vector tu(xref); tu += u;
      gradE.SetSize(op->Height());
      dynamic_cast<ParNonlinearForm*>(op)->Mult(tu, gradE);
   }
   else
   {
      gradE.SetSize(K->Height());
      K->Mult(u, gradE);
   }
   gradE.Add(-1.0, B); 
}

HypreParMatrix * ElasticityOperator::GetHessian(const Vector & u)
{
   if (nonlinear)
   {
      Vector tu(xref); tu += u;
      return dynamic_cast<HypreParMatrix *>(&dynamic_cast<ParNonlinearForm*>(op)->GetGradient(tu));
   }
   else
   {
      return K;
   }
}

ElasticityOperator::~ElasticityOperator()
{
   delete op;
   delete b;
   delete fes;
   delete fec;
   if (K) delete K;
}


OptContactProblem::OptContactProblem(ElasticityOperator * problem_, 
                     const std::set<int> & mortar_attrs_, 
                     const std::set<int> & nonmortar_attrs_,
                     ParGridFunction * coords_, bool doublepass_,
                     const Vector & xref_, 
                     const Vector & xrefbc_, 
                     bool qp_)
: problem(problem_), mortar_attrs(mortar_attrs_), nonmortar_attrs(nonmortar_attrs_),
  coords(coords_), doublepass(doublepass_), xref(xref_), xrefbc(xrefbc_), qp(qp_)
{
   comm = problem->GetComm(); 
   pmesh = problem->GetMesh();
   vfes = problem->GetFESpace(); 
   dim = pmesh->Dimension();
   ComputeGapJacobian();

   dimU = problem->GetNumTDofs();
   dimM = J->Height();
   dimC = J->Height();
   ml.SetSize(dimM); ml = 0.0;
   if (problem->IsNonlinear() && qp)
   {
      energy_ref = problem->GetEnergy(xrefbc);
      problem->GetGradient(xrefbc,grad_ref);
      Kref = problem->GetHessian(xrefbc);
   }
}

void OptContactProblem::ComputeGapJacobian()
{
   Vector gap1;
   HypreParMatrix * J1 = SetupTribol(pmesh,coords,problem->GetEssentialDofs(),
                                     mortar_attrs, nonmortar_attrs,gap1);
   if (doublepass)
   {
      Vector gap2;
      HypreParMatrix * J2 = SetupTribol(pmesh,coords,problem->GetEssentialDofs(),
                                       nonmortar_attrs, mortar_attrs, gap2);
      gapv.SetSize(gap1.Size()+gap2.Size());
      gapv.SetVector(gap1,0);
      gapv.SetVector(gap2,gap1.Size());
      Array2D<HypreParMatrix *> A_array(2,1);
      A_array(0,0) = J1;
      A_array(1,0) = J2;
      J = HypreParMatrixFromBlocks(A_array);
      delete J1;
      delete J2;
   }
   else
   {
      gapv.SetSize(gap1.Size());
      gapv.SetVector(gap1,0);
      J = J1;
   }

   constraints_starts.SetSize(2);
   constraints_starts[0] = J->RowPart()[0];
   constraints_starts[1] = J->RowPart()[1];
}


HypreParMatrix * OptContactProblem::Duuf(const BlockVector & x)
{
   return DddE(x.GetBlock(0));
}

HypreParMatrix * OptContactProblem::Dumf(const BlockVector & x)
{
   return nullptr;
}

HypreParMatrix * OptContactProblem::Dmuf(const BlockVector & x)
{
   return nullptr;
}

HypreParMatrix * OptContactProblem::Dmmf(const BlockVector & x)
{
   return nullptr;
}

HypreParMatrix * OptContactProblem::Duc(const BlockVector & x)
{
   return J;
}

HypreParMatrix * OptContactProblem::Dmc(const BlockVector &)
{
   if (!NegId)
   {
      Vector negone(dimM); negone = -1.0;
      SparseMatrix diag(negone);
      NegId = new HypreParMatrix(comm,J->GetGlobalNumRows(), 
      constraints_starts.GetData(),&diag);
      HypreStealOwnership(*NegId, diag);
   }
   return NegId;
}

HypreParMatrix * OptContactProblem::lDuuc(const BlockVector &, const Vector &)
{
   return nullptr;
}

HypreParMatrix * OptContactProblem::GetRestrictionToInteriorDofs()
{
   if (!Pnc)
   {
      if (!Jt) 
      {
         Jt = J->Transpose();
         Jt->EliminateRows(problem->GetEssentialDofs());
      }

      int hJt = Jt->Height();
      SparseMatrix mergedJt;
      Jt->MergeDiagAndOffd(mergedJt);

      Array<int> zerorows;
      for (int i = 0; i<hJt; i++)
      {
         if (mergedJt.RowIsEmpty(i))
         {
            zerorows.Append(i);
         }
      }

      int hi = zerorows.Size();
      SparseMatrix Pit(hi,vfes->GlobalTrueVSize());

      for (int i = 0; i<hi; i++)
      {
         int col = zerorows[i]+vfes->GetMyTDofOffset();//prob->GetFESpace()->GetMyTDofOffset();
         Pit.Set(i,col,1.0);
      }
      Pit.Finalize();

      int rows_i[2];
      int cols_i[2];
      int nrows_i = Pit.Height();

      int row_offset_i;
      MPI_Scan(&nrows_i,&row_offset_i,1,MPI_INT,MPI_SUM,comm);

      row_offset_i-=nrows_i;
      rows_i[0] = row_offset_i;
      rows_i[1] = row_offset_i+nrows_i;
      for (int i = 0; i < 2; i++)
      {
         cols_i[i] = vfes->GetTrueDofOffsets()[i];
      }
      int glob_nrows_i;
      int glob_ncols_i = vfes->GlobalTrueVSize();
      MPI_Allreduce(&nrows_i, &glob_nrows_i,1,MPI_INT,MPI_SUM,comm);

      HypreParMatrix * P_it = new HypreParMatrix(comm, nrows_i, glob_nrows_i,
                                 glob_ncols_i, Pit.GetI(), Pit.GetJ(),
                                 Pit.GetData(), rows_i,cols_i); 
      
      Pnc = P_it->Transpose();
      delete P_it;
   }

   return Pnc;
}

HypreParMatrix * OptContactProblem::GetRestrictionToContactDofs()
{
   if (!Pc)
   {
      if (!Jt)
      {
         Jt = J->Transpose();
         Jt->EliminateRows(problem->GetEssentialDofs());
      }
      int hJt = Jt->Height();
      SparseMatrix mergedJt;
      Jt->MergeDiagAndOffd(mergedJt);

      Array<int> nonzerorows;
      for (int i = 0; i<hJt; i++)
      {
         if (!mergedJt.RowIsEmpty(i))
         {
            nonzerorows.Append(i);
         }
      }

      int hc = nonzerorows.Size();
      SparseMatrix Pct(hc,vfes->GlobalTrueVSize());

      for (int i = 0; i<hc; i++)
      {
         int col = nonzerorows[i]+vfes->GetMyTDofOffset();
         Pct.Set(i,col,1.0);
      }
      Pct.Finalize();

      int rows_c[2];
      int cols_c[2];
      int nrows_c = Pct.Height();

      int row_offset_c;
      MPI_Scan(&nrows_c,&row_offset_c,1,MPI_INT,MPI_SUM,comm);

      row_offset_c-=nrows_c;
      rows_c[0] = row_offset_c;
      rows_c[1] = row_offset_c+nrows_c;
      for (int i = 0; i < 2; i++)
      {
         cols_c[i] = vfes->GetTrueDofOffsets()[i];
      }
      int glob_nrows_c;
      int glob_ncols_c = vfes->GlobalTrueVSize();
      MPI_Allreduce(&nrows_c, &glob_nrows_c,1,MPI_INT,MPI_SUM,comm);

      HypreParMatrix * P_ct = new HypreParMatrix(comm, nrows_c, glob_nrows_c,
                                                glob_ncols_c, Pct.GetI(), Pct.GetJ(),
                                                Pct.GetData(), rows_c,cols_c); 

      Pc = P_ct->Transpose();
      delete P_ct;                         
   }

   return Pc;
}

void OptContactProblem::c(const BlockVector & x, Vector & y)
{
   Vector temp(x.GetBlock(0).Size()); temp = 0.0;
   temp.Set(1.0, x.GetBlock(0));  
   temp.Add(-1.0, xref); // displacement at previous time step  
   J->Mult(temp, y); // J * (d - xref)
   y.Add(1.0, gapv); // J * (d - xref) + g0 
   y.Add(-1.0, x.GetBlock(1)); // J * (d - xref) + g0 - s
}

real_t OptContactProblem::CalcObjective(const BlockVector & x)
{
   return E(x.GetBlock(0));
}

void OptContactProblem::CalcObjectiveGrad(const BlockVector & x, BlockVector & y)
{
   DdE(x.GetBlock(0), y.GetBlock(0));
   y.GetBlock(1) = 0.0;
}

real_t OptContactProblem::E(const Vector & d)
{
   if (problem->IsNonlinear() && qp)
   {
      // TO DO: compute QP approximation of E
      // (d - xref)^T [ 1/2 K * (d - xref) + gradEQP] + EQP
      double energy = 0.0;
      Vector dx(dimU); dx = 0.0;
      Vector temp(dimU); temp = 0.0;
      dx.Set(1.0, d);
      dx.Add(-1.0, xrefbc);
      Kref->Mult(dx, temp);
      temp *= 0.5;
      temp.Add(1.0, grad_ref);
      energy = InnerProduct(comm, dx, temp);
      energy += energy_ref;
      return energy;
   }
   else
   {
      return problem->GetEnergy(d);
   }
}

void OptContactProblem::DdE(const Vector & d, Vector & gradE)
{
   if (problem->IsNonlinear() && qp)
   {
      // KQP * (d - xref) + gradEQP
      Vector dx(dimU); dx = 0.0;
      dx.Set(1.0, d);
      dx.Add(-1.0, xrefbc);
      Kref->Mult(dx, gradE);
      gradE.Add(1.0, grad_ref);
   }
   else
   {
      return problem->GetGradient(d, gradE);
   }
}

HypreParMatrix * OptContactProblem::DddE(const Vector & d)
{
   if (problem->IsNonlinear() && qp)
   {
      return Kref;
   }
   else
   {
      return problem->GetHessian(d);
   }
}

OptContactProblem::~OptContactProblem()
{

}

HypreParMatrix * SetupTribol(ParMesh * pmesh, ParGridFunction * coords, 
                             const Array<int> & ess_tdofs, const std::set<int> & mortar_attrs, 
                              const std::set<int> & non_mortar_attrs, 
                              Vector &gap)
{
   axom::slic::SimpleLogger logger;
   axom::slic::setIsRoot(mfem::Mpi::Root());

   // Initialize Tribol contact library
   tribol::initialize(pmesh->Dimension(), pmesh->GetComm());

   tribol::parameters_t::getInstance().gap_separation_ratio = 2;

   int coupling_scheme_id = 0;
   int mesh1_id = 0; int mesh2_id = 1;
   
   tribol::registerMfemCouplingScheme(
      coupling_scheme_id, mesh1_id, mesh2_id,
      *pmesh, *coords, mortar_attrs, non_mortar_attrs,
      tribol::SURFACE_TO_SURFACE,
      tribol::NO_SLIDING,
      tribol::SINGLE_MORTAR,
      tribol::FRICTIONLESS,
      tribol::LAGRANGE_MULTIPLIER,
      tribol::BINNING_GRID
   );

   // Access Tribol's pressure grid function (on the contact surface)
   auto& pressure = tribol::getMfemPressure(coupling_scheme_id);
   int vsize = pressure.ParFESpace()->GlobalTrueVSize();
   if (mfem::Mpi::Root())
   {
      std::cout << "Number of pressure unknowns: " <<
                vsize << std::endl;
   }

   // Set Tribol options for Lagrange multiplier enforcement
   tribol::setLagrangeMultiplierOptions(
      coupling_scheme_id,
      tribol::ImplicitEvalMode::MORTAR_RESIDUAL_JACOBIAN
   );

   // Update contact mesh decomposition
   tribol::updateMfemParallelDecomposition();

   // Update contact gaps, forces, and tangent stiffness
   int cycle = 1;   // pseudo cycle
   double t = 1.0;  // pseudo time
   double dt = 1.0; // pseudo dt
   tribol::update(cycle, t, dt);

   // Return contact contribution to the tangent stiffness matrix
   auto A_blk = tribol::getMfemBlockJacobian(coupling_scheme_id);
   
   HypreParMatrix * Mfull = (HypreParMatrix *)(&A_blk->GetBlock(1,0));
   Mfull->EliminateCols(ess_tdofs);

   int h = Mfull->Height();
   SparseMatrix merged;
   Mfull->MergeDiagAndOffd(merged);
   Array<int> nonzero_rows;
   for (int i = 0; i<h; i++)
   {
      if (!merged.RowIsEmpty(i))
      {
         nonzero_rows.Append(i);
      }
   }

   int hnew = nonzero_rows.Size();
   SparseMatrix P(hnew,h);

   for (int i = 0; i<hnew; i++)
   {
      int col = nonzero_rows[i];
      P.Set(i,col,1.0);
   }
   P.Finalize();

   SparseMatrix * reduced_merged = Mult(P,merged);

   int rows[2];
   int cols[2];
   cols[0] = Mfull->ColPart()[0];
   cols[1] = Mfull->ColPart()[1];
   int nrows = reduced_merged->Height();

   int row_offset;
   MPI_Scan(&nrows,&row_offset,1,MPI_INT,MPI_SUM,Mfull->GetComm());

   row_offset-=nrows;
   rows[0] = row_offset;
   rows[1] = row_offset+nrows;
   int glob_nrows;
   MPI_Allreduce(&nrows, &glob_nrows,1,MPI_INT,MPI_SUM,Mfull->GetComm());


   int glob_ncols = reduced_merged->Width();
   HypreParMatrix * M = new HypreParMatrix(Mfull->GetComm(), nrows, glob_nrows,
                          glob_ncols, reduced_merged->GetI(), reduced_merged->GetJ(),
                          reduced_merged->GetData(), rows,cols); 
   delete reduced_merged;                          

   Vector gap_full;
   tribol::getMfemGap(coupling_scheme_id, gap_full);
   auto& P_submesh = *pressure.ParFESpace()->GetProlongationMatrix();
   Vector gap_true(P_submesh.Width());
   P_submesh.MultTranspose(gap_full,gap_true);
   gap.SetSize(nrows);

   for (int i = 0; i<nrows; i++) 
   {
      gap[i] = gap_true[nonzero_rows[i]];
   }
   tribol::finalize();
   return M;
}
