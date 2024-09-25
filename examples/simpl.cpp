//                       MFEM Example 37 - Parallel Version
//
// Compile with: make ex37p
//
// Sample runs:
//    mpirun -np 4 ex37p -alpha 10 -pv
//    mpirun -np 4 ex37p -lambda 0.1 -mu 0.1
//    mpirun -np 4 ex37p -o 2 -alpha 5.0 -mi 50 -vf 0.4 -ntol 1e-5
//    mpirun -np 4 ex37p -r 6 -o 2 -alpha 10.0 -epsilon 0.02 -mi 50 -ntol 1e-5
//
// Description: This example code demonstrates the use of MFEM to solve a
//              density-filtered [3] topology optimization problem. The
//              objective is to minimize the compliance
//
//                  minimize ∫_Ω f⋅u dx over u ∈ [H¹(Ω)]² and ρ ∈ L¹(Ω)
//
//                  subject to
//
//                    -Div(r(ρ̃)Cε(u)) = f       in Ω + BCs
//                    -ϵ²Δρ̃ + ρ̃ = ρ             in Ω + Neumann BCs
//                    0 ≤ ρ ≤ 1                 in Ω
//                    ∫_Ω ρ dx = θ vol(Ω)
//
//              Here, r(ρ̃) = ρ₀ + ρ̃³ (1-ρ₀) is the solid isotropic material
//              penalization (SIMP) law, C is the elasticity tensor for an
//              isotropic linearly elastic material, ϵ > 0 is the design
//              length scale, and 0 < θ < 1 is the volume fraction.
//
//              The problem is discretized and gradients are computing using
//              finite elements [1]. The design is optimized using an entropic
//              mirror descent algorithm introduced by Keith and Surowiec [2]
//              that is tailored to the bound constraint 0 ≤ ρ ≤ 1.
//
//              This example highlights the ability of MFEM to deliver high-
//              order solutions to inverse design problems and showcases how
//              to set up and solve PDE-constrained optimization problems
//              using the so-called reduced space approach.
//
// [1] Andreassen, E., Clausen, A., Schevenels, M., Lazarov, B. S., & Sigmund,
// O.
//     (2011). Efficient topology optimization in MATLAB using 88 lines of
//     code. Structural and Multidisciplinary Optimization, 43(1), 1-16.
// [2] Keith, B. and Surowiec, T. (2023) Proximal Galerkin: A structure-
//     preserving finite element method for pointwise bound constraints.
//     arXiv:2307.12444 [math.NA]
// [3] Lazarov, B. S., & Sigmund, O. (2011). Filters in topology optimization
//     based on Helmholtz‐type differential equations. International Journal
//     for Numerical Methods in Engineering, 86(6), 765-781.

#include "ex37.hpp"
#include "simpl.hpp"
#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

/**
 * @brief Bregman projection of ρ = sigmoid(ψ) onto the subspace
 *        ∫_Ω ρ dx = θ vol(Ω) as follows:
 *
 *        1. Compute the root of the R → R function
 *            f(c) = ∫_Ω sigmoid(ψ + c) dx - θ vol(Ω)
 *        2. Set ψ ← ψ + c.
 *
 * @param psi a GridFunction to be updated
 * @param target_volume θ vol(Ω)
 * @param tol Newton iteration tolerance
 * @param max_its Newton maximum iteration number
 * @return real_t Final volume, ∫_Ω sigmoid(ψ)
 */
real_t proj(ParGridFunction &psi, ParGridFunction &zerogf,
            real_t volume_fraction, real_t domain_volume, real_t tol = 1e-12,
            int max_its = 10)
{
   real_t target_volume = domain_volume * volume_fraction;

   // Solution bracket
   real_t a = inv_sigmoid(volume_fraction) - psi.Max();
   MPI_Allreduce(MPI_IN_PLACE, &a, 1, MFEM_MPI_REAL_T, MPI_MIN,
                 psi.ParFESpace()->GetComm());
   real_t b = inv_sigmoid(volume_fraction) - psi.Min();
   MPI_Allreduce(MPI_IN_PLACE, &b, 1, MFEM_MPI_REAL_T, MPI_MAX,
                 psi.ParFESpace()->GetComm());

   // Current testing iterate
   real_t s(0), vs(mfem::infinity());
   MappedGridFunctionCoefficient sigmoid_psi(
   &psi, [&s](const real_t x) { return sigmoid(x + s); });

   // If psi is constant everywhere, there is nothing to do. Return current volume
   if (a==b) { return zerogf.ComputeL1Error(sigmoid_psi); }

   // Compute bracket function value
   s = a;
   real_t va = zerogf.ComputeL1Error(sigmoid_psi) - target_volume;
   s = b;
   real_t vb = zerogf.ComputeL1Error(sigmoid_psi) - target_volume;

   // Auxiliary variables
   real_t c = a;
   real_t vc = va;
   real_t d = c;
   bool mflag = true;

   // Brent's method
   while (std::fabs(b - a) > tol && std::fabs(vs)>tol)
   {
      if (std::fabs(va-vc) > 1e-08 && std::fabs(vb-vc) > 1e-08)
      {
         // inverse quadratic interpolation
         s = a*vb*vc/((va-vb)*(va-vc))+b*va*vc/((vb-va)*(vb-vc))+c*va*vb/((vc-va)*
                                                                          (vc-vb));
      }
      else
      {
         // Secant method
         s = b - vb*(b-a)/(vb-va);
      }
      // conditions to be checked
      bool cond1 = (s>(3*a+b)/4.0 && s<b) || (s>b && s<(3*a+b)/4.0);
      bool cond2 = mflag && std::fabs(s-b) >= std::fabs(b-c)/2.0;
      bool cond3 = !mflag && std::fabs(s-b) >= std::fabs(c-d)/2.0;
      bool cond4 = mflag && std::fabs(b-c) < tol;
      bool cond5 = !mflag && std::fabs(c-d) < tol;
      if (cond1 || cond2 || cond3 || cond4 || cond5)
      {
         // use bisection
         s = (a+b)*0.5;
         mflag = true;
      }
      else
      {
         // use better ones
         mflag = false;
      }
      // Update current value
      vs = zerogf.ComputeL1Error(sigmoid_psi) - target_volume;

      // Update iterates
      d = c;
      c = b; vc=vb;
      if (va*vs < 0) {b = s; vb = vs;}
      else {a=s; va=vs;}
      if (std::fabs(va) < std::fabs(vb))
      {
         real_t temp = b;
         b = a; a = temp;
         temp = vb;
         vb = va; va = temp;
      }
   }
   psi += s;
   return vs+target_volume;
}

class TableLogger
{
public:
   enum dtype { DOUBLE, INT };

protected:
   // Double data to be printed.
   std::vector<double *> data_double;
   // Int data to be printed
   std::vector<int *> data_int;
   // Data type for each column
   std::vector<dtype> data_order;
   // Name of each monitored data
   std::vector<std::string> names;
   // Output stream
   std::ostream &os;
   // Column width
   int w;
   // Whether the variable name row has been printed or not
   bool var_name_printed;
   bool isRoot; // true if serial or root in parallel
   std::unique_ptr<std::ofstream> file;

private:
public:
   // Create a logger that prints a row of variables for each call of Print
   TableLogger(std::ostream &os = mfem::out);
   // Set column width of the table to be printed
   void setw(const int column_width) { w = column_width; }
   // Add double data to be monitored
   void Append(const std::string name, double &val);
   // Add double data to be monitored
   void Append(const std::string name, int &val);
   // Print a row of currently monitored data. If it is called
   void Print(bool print_valname=false);
   // Save data to a file whenever Print is called.
   void SaveWhenPrint(std::string filename,
                      std::ios::openmode mode = std::ios::out);
   // Close file manually.
   void CloseFile()
   {
      if (file)
      {
         file.reset(nullptr);
      }
   }
};
/*
 * ---------------------------------------------------------------
 *                      ALGORITHM PREAMBLE
 * ---------------------------------------------------------------
 *
 *  The Lagrangian for this problem is
 *
 *          L(u,ρ,ρ̃,w,w̃) = (f,u) - (r(ρ̃) C ε(u),ε(w)) + (f,w)
 *                       - (ϵ² ∇ρ̃,∇w̃) - (ρ̃,w̃) + (ρ,w̃)
 *
 *  where
 *
 *    r(ρ̃) = ρ₀ + ρ̃³ (1 - ρ₀)       (SIMP rule)
 *
 *    ε(u) = (∇u + ∇uᵀ)/2           (symmetric gradient)
 *
 *    C e = λtr(e)I + 2μe           (isotropic material)
 *
 *  NOTE: The Lame parameters can be computed from Young's modulus E
 *        and Poisson's ratio ν as follows:
 *
 *             λ = E ν/((1+ν)(1-2ν)),      μ = E/(2(1+ν))
 *
 * ---------------------------------------------------------------
 *
 *  Discretization choices:
 *
 *     u ∈ V ⊂ (H¹)ᵈ (order p)
 *     ψ ∈ L² (order p - 1), ρ = sigmoid(ψ)
 *     ρ̃ ∈ H¹ (order p)
 *     w ∈ V  (order p)
 *     w̃ ∈ H¹ (order p)
 *
 * ---------------------------------------------------------------
 *                          ALGORITHM
 * ---------------------------------------------------------------
 *
 *  Update ρ with projected mirror descent via the following algorithm.
 *
 *  1. Initialize ψ = inv_sigmoid(vol_fraction) so that ∫ sigmoid(ψ) = θ vol(Ω)
 *
 *  While not converged:
 *
 *     2. Solve filter equation ∂_w̃ L = 0; i.e.,
 *
 *           (ϵ² ∇ ρ̃, ∇ v ) + (ρ̃,v) = (ρ,v)   ∀ v ∈ H¹.
 *
 *     3. Solve primal problem ∂_w L = 0; i.e.,
 *
 *      (λ r(ρ̃) ∇⋅u, ∇⋅v) + (2 μ r(ρ̃) ε(u), ε(v)) = (f,v)   ∀ v ∈ V.
 *
 *     NB. The dual problem ∂_u L = 0 is the negative of the primal problem due
 * to symmetry.
 *
 *     4. Solve for filtered gradient ∂_ρ̃ L = 0; i.e.,
 *
 *      (ϵ² ∇ w̃ , ∇ v ) + (w̃ ,v) = (-r'(ρ̃) ( λ |∇⋅u|² + 2 μ |ε(u)|²),v)   ∀ v ∈
 * H¹.
 *
 *     5. Project the gradient onto the discrete latent space; i.e., solve
 *
 *                         (G,v) = (w̃,v)   ∀ v ∈ L².
 *
 *     6. Bregman proximal gradient update; i.e.,
 *
 *                            ψ ← ψ - αG + c,
 *
 *     where α > 0 is a step size parameter and c ∈ R is a constant ensuring
 *
 *                     ∫_Ω sigmoid(ψ - αG + c) dx = θ vol(Ω).
 *
 *  end
 */

int main(int argc, char *argv[])
{
   // 0. Initialize MPI and HYPRE.
   Mpi::Init();
   int num_procs = Mpi::WorldSize();
   int myid = Mpi::WorldRank();
   Hypre::Init();

   // 1. Parse command-line options.
   int ref_levels = 2;
   int par_ref_levels = 4;
   int order = 1;
   real_t alpha = 1.0;
   // real_t epsilon = 0.01;
   real_t filter_radius = -1;
   real_t vol_fraction = -1;
   int max_it = 1e3;
   int max_backtrack = 1e2;
   real_t tol_stationarity = 1e-06;
   real_t tol_compliance = 1e-05;
   bool stationarity_in_Bregman = true;
   bool backtrack_bregman = true;
   real_t rho_min = 1e-6;
   real_t lambda = 1.0;
   real_t mu = 1.0;
   bool glvis_visualization = true;
   bool paraview_output = true;
   int problem = 1;

   OptionsParser args(argc, argv);
   args.AddOption(&problem, "-p", "--problem",
                  "Topology Problem:\n\t"
                  "1 : Cantilever 2D\n\t"
                  "2 : Cantilever 3D\n\t"
                  "3 : MBB 2D\n\t"
                  "4 : Torsion 3D\n\t"
                 );
   args.AddOption(&ref_levels, "-rs", "--refine",
                  "Number of times to refine the mesh uniformly.");
   args.AddOption(&ref_levels, "-rp", "--refine-parallel",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite elements.");
   args.AddOption(&alpha, "-alpha", "--alpha-step-length",
                  "Step length for gradient descent.");
   args.AddOption(&filter_radius, "-fr", "--filter-radius",
                  "Filter radius for Helmholtz filter. eps = filter_radius/sqrt(12)");
   args.AddOption(&max_it, "-mi", "--max-it",
                  "Maximum number of gradient descent iterations.");
   args.AddOption(&max_backtrack, "-mi-back", "--max-backtrack",
                  "Maximum number of backtracking iteration");
   args.AddOption(&tol_stationarity, "-tol-s", "--tol-stationarity",
                  "Tolerance for Stationarity Error");
   args.AddOption(&tol_compliance, "-tol-c", "--tol-compliance",
                  "Tolerance for relative compliance decrease");
   args.AddOption(&vol_fraction, "-vf", "--volume-fraction",
                  "Volume fraction for the material density.");
   args.AddOption(&lambda, "-lambda", "--lambda", "Lamé constant λ.");
   args.AddOption(&mu, "-mu", "--mu", "Lamé constant μ.");
   args.AddOption(&rho_min, "-rmin", "--psi-min",
                  "Minimum of density coefficient.");
   args.AddOption(&glvis_visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&paraview_output, "-pv", "--paraview", "-no-pv",
                  "--no-paraview", "Enable or disable ParaView output.");
   args.AddOption(&stationarity_in_Bregman, "-cb", "--converge-bregman", "-c2",
                  "--converge-L2", "Check stationarity convergence in Bregman or L2");
   args.AddOption(&backtrack_bregman, "-bb", "--backtrack-bregman", "-ba",
                  "--backtrack-armijo", "Backtracking using Bregman or Armijo");
   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }

   Array2D<int> ess_bdr;
   ParMesh pmesh = GetParMeshTopopt((TopoptProblem)problem,
                                    ref_levels, par_ref_levels,
                                    filter_radius, vol_fraction,
                                    ess_bdr);
   if (myid == 0)
   {
      mfem::out << num_procs << " number of process created.\n";
      args.PrintOptions(cout);
   }

   int dim = pmesh.Dimension();

   // 4. Define the necessary finite element spaces on the mesh.
   H1_FECollection state_fec(order, dim);  // space for u
   H1_FECollection filter_fec(order, dim); // space for ρ̃
   L2_FECollection control_fec(order - 1, dim); // space for ψ
   ParFiniteElementSpace state_fes(&pmesh, &state_fec, dim);
   ParFiniteElementSpace filter_fes(&pmesh, &filter_fec);
   ParFiniteElementSpace control_fes(&pmesh, &control_fec);

   HYPRE_BigInt state_size = state_fes.GlobalTrueVSize();
   HYPRE_BigInt control_size = control_fes.GlobalTrueVSize();
   HYPRE_BigInt filter_size = filter_fes.GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of state unknowns: " << state_size << endl;
      cout << "Number of filter unknowns: " << filter_size << endl;
      cout << "Number of control unknowns: " << control_size << endl;
   }

   // 5. Set the initial guess for ρ.
   ParGridFunction u(&state_fes);
   u = 0.0;
   ParGridFunction psi(&control_fes);
   ParGridFunction psi_old(&control_fes);
   ParGridFunction psi_eps(&control_fes); // forcomputing stationarity
   ParGridFunction rho_filter(&filter_fes);
   ParGridFunction rho_gf(&control_fes);
   ParGridFunction grad(&control_fes);
   grad = 0.0;
   ParGridFunction w_filter(&filter_fes);
   w_filter = 0.0;
   ParGridFunction zerogf(&control_fes);
   zerogf = 0.0;
   ParGridFunction grad_old(grad);
   grad_old = 0.0;

   // ρ = sigmoid(ψ)
   MappedGridFunctionCoefficient rho(&psi, sigmoid);
   GridFunctionCoefficient rho_filter_cf(&rho_filter);
   // Interpolation of ρ = sigmoid(ψ) in control fes (for ParaView output)
   // ρ - ρ_old = sigmoid(ψ) - sigmoid(ψ_old)
   DiffMappedGridFunctionCoefficient succ_diff_rho(&psi, &psi_old, sigmoid);
   DiffMappedGridFunctionCoefficient diff_rho_rhoeps(&psi_eps, &psi, sigmoid);
   MappedPairedGridFunctionCoefficient succ_diff_rho_bregman(&psi, &psi_old,
                                                             bregman_divergence_latent);
   MappedPairedGridFunctionCoefficient diff_rho_rhoeps_bregman(&psi_eps, &psi,
                                                               bregman_divergence_latent);
   ParLinearForm succ_diff_rho_form(&control_fes);
   succ_diff_rho_form.AddDomainIntegrator(new DomainLFIntegrator(succ_diff_rho));

   // 6. Set-up the physics solver.
   ConstantCoefficient one(1.0);
   ConstantCoefficient lambda_cf(lambda);
   ConstantCoefficient mu_cf(mu);
   SIMPInterpolationCoefficient SIMP_cf(&rho_filter, rho_min, 1.0);
   ProductCoefficient lambda_SIMP_cf(lambda, SIMP_cf);
   ProductCoefficient mu_SIMP_cf(mu, SIMP_cf);
   StrainEnergyDensityCoefficient energy(&lambda_cf, &mu_cf, &u, &rho_filter,
                                         rho_min);

   LinearElasticityProblem ElasticitySolver(state_fes, &lambda_SIMP_cf,
                                            &mu_SIMP_cf, false);
   SetupTopoptProblem((TopoptProblem)problem, ElasticitySolver, rho_filter_cf);
   ElasticitySolver.SetEssentialBoundary(ess_bdr);
   ElasticitySolver.SetBstationary();
   ElasticitySolver.AssembleStationaryOperators();

   psi = inv_sigmoid(vol_fraction);
   psi_old = inv_sigmoid(vol_fraction);
   psi_eps = inv_sigmoid(vol_fraction);
   rho_filter = vol_fraction;
   rho_gf = vol_fraction;

   // 7. Set-up the filter solver.
   HelmholtzFilter FilterSolver(filter_fes, filter_radius, &rho, &energy);
   FilterSolver.SetAstationary();
   FilterSolver.AssembleStationaryOperators();

   // 8. Define the Lagrange multiplier and gradient functions.

   GridFunctionCoefficient w_filter_cf(&w_filter);
   L2Projector L2projector(control_fes, &w_filter_cf);
   FilterSolver.SetAstationary();
   L2projector.AssembleStationaryOperators();

   // 9. Define some tools for later.
   real_t domain_volume = zerogf.ComputeL1Error(one);

   // 10. Connect to GLVis. Prepare for VisIt output.
   char vishost[] = "localhost";
   int visport = 19916;
   socketstream sout_filter, sout_rho, sout_u;
   if (glvis_visualization)
   {
      sout_filter.open(vishost, visport);
      sout_filter.precision(8);
      sout_rho.open(vishost, visport);
      sout_rho.precision(8);
      sout_u.open(vishost, visport);
      sout_u.precision(8);
   }

   mfem::ParaViewDataCollection paraview_dc("ex37p", &pmesh);
   if (paraview_output)
   {
      rho_gf.ProjectCoefficient(rho);
      paraview_dc.SetPrefixPath("ParaView");
      paraview_dc.SetLevelsOfDetail(order);
      paraview_dc.SetDataFormat(VTKFormat::BINARY);
      paraview_dc.SetHighOrderOutput(true);
      paraview_dc.SetCycle(0);
      paraview_dc.SetTime(0.0);
      paraview_dc.RegisterField("displacement", &u);
      paraview_dc.RegisterField("density", &rho_gf);
      paraview_dc.RegisterField("filtered_density", &rho_filter);
      paraview_dc.Save();
   }
   TableLogger logger;
   real_t material_volume(infinity()), compliance(infinity()),
          stationarityError(infinity()), stationarityBregmanError(infinity());
   real_t succ_compliance_diff;
   int num_reeval(-1);
   std::string filename_prefix;
   // filename_prefix.append("PMD-Cantilever2");
   filename_prefix.append("PMD-Cantilever3");
   // filename_prefix.append("PMD-Torsion");
   logger.Append(std::string("Volume"), material_volume);
   logger.Append(std::string("Compliance"), compliance);
   logger.Append(std::string("Stationarity-2"), stationarityError);
   logger.Append(std::string("Stationarity-B"), stationarityBregmanError);
   logger.Append(std::string("Re-evel"), num_reeval);
   logger.Append(std::string("Step Size"), alpha);
   logger.Append(std::string("Succ-Obj-Diff"), succ_compliance_diff);
   logger.SaveWhenPrint(filename_prefix);

   // 11. Iterate:
   alpha = 1.0;
   real_t compliance_old;
   int total_num_feval(0), total_num_geval(0);
   for (int k = 1; k <= max_it; k++)
   {
      if (myid == 0)
      {
         cout << "\nStep = " << k << endl;
      }
      if (k > 2)
      {
         succ_diff_rho_form.Assemble();
         psi_old -= psi;
         grad_old -= grad;
         real_t numer = -InnerProduct(MPI_COMM_WORLD, psi_old, succ_diff_rho_form);
         real_t denomi =
            -InnerProduct(MPI_COMM_WORLD, grad_old, succ_diff_rho_form);
         alpha = std::fabs(numer / denomi);
         if (Mpi::Root())
         {
            mfem::out << "step size: " << alpha << " = " << numer << " / " << denomi
                      << std::endl;
         }
      }

      compliance_old = compliance;
      psi_old = psi;
      if (Mpi::Root())
      {
         std::cout << "Backtracking Starts" << std::endl;
      }
      // Backtracking line search
      for (num_reeval = 0; num_reeval < max_backtrack; num_reeval++)
      {
         total_num_feval++;
         if (Mpi::Root()) { std::cout << "\tAttempt " << num_reeval+1 << std::endl; }
         // update psi
         psi = psi_old;
         psi.Add(-alpha, grad);
         // Bregman projection for volume constraint
         material_volume = proj(psi, zerogf, vol_fraction, domain_volume);
         if (Mpi::Root()) { cout << "\t\tVolume Projection done" << std::endl; }

         // Step 1 - Filter solve
         // Solve (ϵ^2 ∇ ρ̃, ∇ v ) + (ρ̃,v) = (ρ,v)
         FilterSolver.Solve(rho_filter, false, true);
         if (Mpi::Root()) { cout << "\t\tFilter Solve done" << std::endl; }

         // Step 2 - State solve
         // Solve (λ r(ρ̃) ∇⋅u, ∇⋅v) + (2 μ r(ρ̃) ε(u), ε(v)) = (f,v)
         ElasticitySolver.Solve(u, true, false);
         if (Mpi::Root()) { cout << "\t\tElasticity Solve done" << std::endl; }

         compliance = (ElasticitySolver.GetLinearForm())(u);
         MPI_Allreduce(MPI_IN_PLACE, &compliance, 1, MPITypeMap<real_t>::mpi_type,
                       MPI_SUM, MPI_COMM_WORLD);
         succ_diff_rho_form.Assemble();
         real_t directional_derval = InnerProduct(MPI_COMM_WORLD, grad,
                                                  succ_diff_rho_form);
         real_t succ_bregman = zerogf.ComputeL1Error(succ_diff_rho_bregman);
         if (Mpi::Root())
         {
            cout << "\t\tNew Compliance    : " << compliance << std::endl;
            if (backtrack_bregman)
            {
               cout << "\t\tTarget Compliance : " << compliance_old + directional_derval +
                    succ_bregman/alpha <<
                    " = " << compliance_old << " + " << directional_derval << " + " << succ_bregman
                    << " / " << alpha << std::endl;
            }
            else
            {
               cout << "\t\tTarget Compliance : " << compliance_old + 1e-04*directional_derval
                    <<
                    " = " << compliance_old << " + 10^-4*" << directional_derval << std::endl;

            }
         }
         if (compliance < compliance_old + 1e-04*directional_derval)
         {
            if (Mpi::Root())
            {
               std::cout << "\tBacktracking finished with " << num_reeval << " failures" <<
                         std::endl;
            }
            break;
         }
         alpha *= 0.5;
      }

      if (glvis_visualization)
      {
         sout_filter << "parallel " << num_procs << " " << myid << "\n";
         sout_filter << "solution\n"
                     << pmesh << rho_filter << "window_title 'Filtered density ρ̃'"
                     << flush;
         rho_gf.ProjectCoefficient(rho);
         sout_rho << "parallel " << num_procs << " " << myid << "\n";
         sout_rho << "solution\n"
                  << pmesh << rho_gf << "window_title 'Control density rho'"
                  << flush;
         sout_u << "parallel " << num_procs << " " << myid << "\n";
         sout_u << "solution\n"
                << pmesh << u << "window_title 'Velocity Magnitude u'"
                << flush;
      }

      if (paraview_output)
      {
         rho_gf.ProjectCoefficient(rho);
         paraview_dc.SetCycle(0);
         paraview_dc.SetTime((real_t)0);
         paraview_dc.Save();
      }

      total_num_geval++;
      if (Mpi::Root()) { cout << "Updating Gradient" << std::endl; }
      // Update gradient
      grad_old = grad;
      FilterSolver.SolveDual(w_filter, false, true);
      if (Mpi::Root()) { cout << "\tDual Filter Solve done" << std::endl; }

      L2projector.Solve(grad, false, true);
      if (Mpi::Root()) { cout << "\tL2 Projection of Gradient done" << std::endl; }

      // Stationarity error
      psi_eps = psi;
      psi_eps.Add(-1e-03, grad);
      proj(psi_eps, zerogf, vol_fraction, domain_volume);
      stationarityError = zerogf.ComputeL2Error(diff_rho_rhoeps) / 1e-03;
      stationarityBregmanError = std::sqrt(zerogf.ComputeL1Error(
                                              diff_rho_rhoeps_bregman)) / 1e-03;
      succ_compliance_diff = (compliance_old - compliance) / std::fabs(compliance);

      logger.Print(true);

      bool isStationarityPoint = stationarity_in_Bregman
                                 ? (stationarityBregmanError < tol_stationarity)
                                 : (stationarityError < tol_stationarity);
      bool objConverged = (compliance_old - compliance) / std::fabs(
                             compliance) < tol_compliance;
      if (isStationarityPoint && objConverged)
      {
         break;
      }
   }
   logger.CloseFile();
   if (Mpi::Root())
   {
      mfem::out << "SiMPL-" << (backtrack_bregman ? "B" : "A") << " Done" <<
                std::endl;
      mfem::out << "\tTotal Function Evaluation: " << total_num_feval << std::endl;
      mfem::out << "\tTotal Gradient Evaluation: " << total_num_geval << std::endl;
   }

   return 0;
}

TableLogger::TableLogger(std::ostream &os)
   : os(os), w(10), var_name_printed(false), isRoot(true)
{
#ifdef MFEM_USE_MPI
   isRoot = Mpi::IsInitialized() ? Mpi::Root() : true;
#endif
}

void TableLogger::Append(const std::string name, double &val)
{
   names.push_back(name);
   data_double.push_back(&val);
   data_order.push_back(dtype::DOUBLE);
}

void TableLogger::Append(const std::string name, int &val)
{
   names.push_back(name);
   data_int.push_back(&val);
   data_order.push_back(dtype::INT);
}

void TableLogger::Print(bool print_varname)
{
   if (isRoot)
   {
      if (!var_name_printed || print_varname)
      {
         for (auto &name : names)
         {
            os << std::setw(w) << name << "\t";
         }
         os << "\b\b";
         os << "\n";
         if (!var_name_printed && file && file->is_open())
         {
            for (auto &name : names)
            {
               *file << std::setw(w) << name << "\t";
            }
            *file << std::endl;
         }
         var_name_printed = true;
      }
      int i_double(0), i_int(0);
      for (auto d : data_order)
      {
         switch (d)
         {
            case dtype::DOUBLE:
            {
               os << std::setw(w) << *data_double[i_double] << ",\t";
               if (file && file->is_open())
               {
                  *file << std::setprecision(8) << std::scientific
                        << *data_double[i_double] << ",\t";
               }
               i_double++;
               break;
            }
            case dtype::INT:
            {
               os << std::setw(w) << *data_int[i_int] << ",\t";
               if (file && file->is_open())
               {
                  *file << *data_int[i_int] << ",\t";
               }
               i_int++;
               break;
            }
            default:
            {
               MFEM_ABORT("Unknown data type. See, TableLogger::dtype");
            }
         }
      }
      os << "\b\b"; // remove the last ,\t
      os << std::endl;
      if (file)
      {
         *file << std::endl;
      }
   }
}
void TableLogger::SaveWhenPrint(std::string filename, std::ios::openmode mode)
{
   if (isRoot)
   {
      filename = filename.append(".txt");
      file.reset(new std::ofstream);
      file->open(filename, mode);
      if (!file->is_open())
      {
         std::string msg("");
         msg += "Cannot open file ";
         msg += filename;
         MFEM_ABORT(msg);
      }
   }
}
