
#ifndef MFEM_LPQ_JACOBI_HPP
#define MFEM_LPQ_JACOBI_HPP

#include "mfem.hpp"
#include "miniapps/common/mfem-common.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

namespace lpq_jacobi
{

int NDIGITS = 20;
int MAX_ITER = 100;
real_t REL_TOL = 1e-4;

// Enumerator for the different solvers to implement
enum SolverType
{
   sli,
   cg,
   num_solvers,  // last
};

// Enumerator for the different integrators to implement
enum IntegratorType
{
   mass,
   diffusion,
   elasticity,
   maxwell,
   num_integrators,  // last
};

// Custom monitor that prints a csv-like file
class DataMonitor : public IterativeSolverMonitor
{
private:
   ofstream os;
   int precision;
public:
   DataMonitor(string file_name, int ndigits) : os(file_name), precision(ndigits)
   {
      if (Mpi::Root())
      {
         mfem::out << "Saving iterations into: " << file_name << endl;
      }
      os << "it,res,sol" << endl;
      os << fixed << setprecision(precision);
   }
   void MonitorResidual(int it, real_t norm, const Vector &x, bool final)
   {
      os << it << "," << norm << ",";
   }
   void MonitorSolution(int it, real_t norm, const Vector &x, bool final)
   {
      os << norm << endl;
   }
};

// Custom general geometric multigrid method, derived from GeometricMultigrid
class GeneralGeometricMultigrid : public GeometricMultigrid
{
public:
   // Constructor
   GeneralGeometricMultigrid(ParFiniteElementSpaceHierarchy& fes_hierarchy,
                             Array<int>& ess_bdr,
                             IntegratorType it,
                             SolverType st,
                             real_t p_order,
                             real_t q_order)
      : GeometricMultigrid(fes_hierarchy, ess_bdr),
        one(1.0),
        coarse_solver(nullptr),
        coarse_pc(nullptr),
        integrator_type(it),
        solver_type(st),
        p_order(p_order),
        q_order(q_order)
   {
      ConstructCoarseOperatorAndSolver(fes_hierarchy.GetFESpaceAtLevel(0));
      for (int l = 1; l < fes_hierarchy.GetNumLevels(); ++l)
      {
         ConstructOperatorAndSmoother(fes_hierarchy.GetFESpaceAtLevel(l), l);
      }
   }

   ~GeneralGeometricMultigrid()
   {
      delete coarse_pc;
   }

private:
   real_t p_order;
   real_t q_order;
   ConstantCoefficient one;
   Solver* coarse_solver;
   OperatorLpqJacobiSmoother* coarse_pc;
   SolverType solver_type;
   IntegratorType integrator_type;

   void ConstructCoarseOperatorAndSolver(ParFiniteElementSpace& coarse_fespace)
   {
      ConstructBilinearForm(coarse_fespace, false);

      HypreParMatrix* coarse_mat = new HypreParMatrix();
      bfs[0]->FormSystemMatrix(*essentialTrueDofs[0], *coarse_mat);

      switch (solver_type)
      {
         case sli:
            coarse_solver = new SLISolver(MPI_COMM_WORLD);
            break;
         case cg:
            coarse_solver = new CGSolver(MPI_COMM_WORLD);
            break;
         default:
            mfem_error("Invalid solver type!");
      }

      coarse_pc = new OperatorLpqJacobiSmoother(*coarse_mat,
                                                *essentialTrueDofs[0],
                                                p_order,
                                                q_order);

      IterativeSolver *it_solver = dynamic_cast<IterativeSolver *>(coarse_solver);
      if (it_solver)
      {
         it_solver->SetRelTol(REL_TOL);
         it_solver->SetMaxIter(MAX_ITER);
         it_solver->SetPrintLevel(1);
         it_solver->SetPreconditioner(*coarse_pc);
         // it_solver->SetMonitor(monitor);
      }
      coarse_solver->SetOperator(*coarse_mat);

      // Last two variables transfer ownership of the pointers
      // Operator and solver
      AddLevel(coarse_mat, coarse_solver, true, true);
   }

   void ConstructOperatorAndSmoother(ParFiniteElementSpace& fespace, int level)
   {
      const Array<int> &ess_tdof_list = *essentialTrueDofs[level];
      ConstructBilinearForm(fespace, true);

      OperatorPtr opr;
      // opr.SetType(Operator::ANY_TYPE);
      opr.SetType(Operator::Hypre_ParCSR);
      bfs.Last()->FormSystemMatrix(ess_tdof_list, opr);
      opr.SetOperatorOwner(false);

      // *opr, diag, ess_tdof_list, 2, fespace.GetParMesh()->GetComm());
      Solver* smoother = new OperatorLpqJacobiSmoother(*opr.As<HypreParMatrix>(),
                                                       ess_tdof_list,
                                                       p_order,
                                                       q_order);
      AddLevel(opr.Ptr(), smoother, true, true);
   }


   // Put later
   void ConstructBilinearForm(ParFiniteElementSpace& fespace,
                              bool partial_assembly = true)
   {
      ParBilinearForm* form = new ParBilinearForm(&fespace);

      if (partial_assembly)
      {
         form->SetAssemblyLevel(AssemblyLevel::PARTIAL);
      }

      switch (integrator_type)
      {
         case mass:
            form->AddDomainIntegrator(new MassIntegrator);
            break;
         case diffusion:
            form->AddDomainIntegrator(new DiffusionIntegrator);
            break;
         case elasticity:
            form->AddDomainIntegrator(new ElasticityIntegrator(one, one));
            break;
         case maxwell:
            form->AddDomainIntegrator(new CurlCurlIntegrator(one));
            form->AddDomainIntegrator(new VectorFEMassIntegrator(one));
            break;
         default:
            mfem_error("Invalid integrator type! Check ParBilinearForm");
      }
      form->Assemble();
      bfs.Append(form);
   }
};

}
#endif // MFEM_LPQ_JACOBI_HPP
