// Copyright (c) 2010-2024, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "tmop_pa.hpp"

namespace mfem
{

class TMOPSetupGradPA2D
{
   const mfem::TMOP_Integrator *ti; // not owned
   const Vector &x;

public:
   TMOPSetupGradPA2D(const TMOP_Integrator *ti, const Vector &x): ti(ti), x(x) { }

   int Ndof() const { return ti->PA.maps->ndof; }

   int Nqpt() const { return ti->PA.maps->nqpt; }

   template<typename METRIC, int T_D1D = 0, int T_Q1D = 0, int T_MAX = 4>
   void operator()()
   {
      constexpr int DIM = 2, NBZ = 1;
      const double metric_normal = ti->metric_normal;
      const int NE = ti->PA.ne, d = ti->PA.maps->ndof, q = ti->PA.maps->nqpt;

      Array<double> mp;
      if (auto m = dynamic_cast<TMOP_Combo_QualityMetric *>(ti->metric))
      {
         m->GetWeights(mp);
      }
      const double *w = mp.Read();

      const auto B = Reshape(ti->PA.maps->B.Read(), q,d);
      const auto G = Reshape(ti->PA.maps->G.Read(), q,d);
      const auto W = Reshape(ti->PA.ir->GetWeights().Read(), q,q);
      const auto J = Reshape(ti->PA.Jtr.Read(), DIM,DIM, q,q, NE);
      const auto X = Reshape(x.Read(), d,d, DIM, NE);
      auto H = Reshape(ti->PA.H.Write(), DIM,DIM, DIM,DIM, q,q, NE);

      const int Q1D = T_Q1D ? T_Q1D : q;

      mfem::forall_2D_batch(NE, Q1D, Q1D, NBZ, [=] MFEM_HOST_DEVICE (int e)
      {
         const int D1D = T_D1D ? T_D1D : d;
         const int Q1D = T_Q1D ? T_Q1D : d;
         constexpr int NBZ = 1;
         constexpr int MQ1 = T_Q1D ? T_Q1D : T_MAX;
         constexpr int MD1 = T_D1D ? T_D1D : T_MAX;

         MFEM_SHARED double s_BG[2][MQ1*MD1];
         MFEM_SHARED double s_X[2][NBZ][MD1*MD1];
         MFEM_SHARED double s_DQ[4][NBZ][MD1*MQ1];
         MFEM_SHARED double s_QQ[4][NBZ][MQ1*MQ1];

         kernels::internal::LoadX<MD1,NBZ>(e,D1D,X,s_X);
         kernels::internal::LoadBG<MD1,MQ1>(D1D, Q1D, B, G, s_BG);

         kernels::internal::GradX<MD1,MQ1,NBZ>(D1D, Q1D, s_BG, s_X, s_DQ);
         kernels::internal::GradY<MD1,MQ1,NBZ>(D1D, Q1D, s_BG, s_DQ, s_QQ);

         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               const double *Jtr = &J(0,0,qx,qy,e);
               const double detJtr = kernels::Det<2>(Jtr);
               const double weight = metric_normal * W(qx,qy) * detJtr;

               // Jrt = Jtr^{-1}
               double Jrt[4];
               kernels::CalcInverse<2>(Jtr, Jrt);

               // Jpr = X^t.DSh
               double Jpr[4];
               kernels::internal::PullGrad<MQ1,NBZ>(Q1D,qx,qy,s_QQ,Jpr);

               // Jpt = Jpr.Jrt
               double Jpt[4];
               kernels::Mult(2,2,2, Jpr, Jrt, Jpt);

               // metric->AssembleH
               METRIC{}.AssembleH(qx,qy,e, weight, Jpt, w,H);
            } // qx
         } // qy
      });
   }
};

} // namespace mfem
