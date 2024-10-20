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

class TMOPAddMultPA3D
{
   const mfem::TMOP_Integrator *ti; // not owned
   const Vector &x;
   Vector &y;

public:
   TMOPAddMultPA3D(const TMOP_Integrator *ti, const Vector &x, Vector &y):
      ti(ti), x(x), y(y) { }

   int Ndof() const { return ti->PA.maps->ndof; }

   int Nqpt() const { return ti->PA.maps->nqpt; }

   template<typename METRIC, int T_D1D = 0, int T_Q1D = 0, int T_MAX = 4>
   void operator()()
   {
      constexpr int DIM = 3;
      const double metric_normal = ti->metric_normal;
      const int NE = ti->PA.ne, d = ti->PA.maps->ndof, q = ti->PA.maps->nqpt;

      Array<double> mp;
      if (auto m = dynamic_cast<TMOP_Combo_QualityMetric *>(ti->metric))
      {
         m->GetWeights(mp);
      }
      const double *w = mp.Read();

      const auto J = Reshape(ti->PA.Jtr.Read(), DIM,DIM, q,q,q, NE);
      const auto W = Reshape(ti->PA.ir->GetWeights().Read(), q,q,q);
      const auto B = Reshape(ti->PA.maps->B.Read(), q,d);
      const auto G = Reshape(ti->PA.maps->G.Read(), q,d);
      const auto X = Reshape(x.Read(), d,d,d, DIM, NE);
      auto Y = Reshape(y.ReadWrite(), d,d,d, DIM, NE);

      const int Q1D = T_Q1D ? T_Q1D : q;

      mfem::forall_3D(NE, Q1D, Q1D, Q1D, [=] MFEM_HOST_DEVICE (int e)
      {
         const int D1D = T_D1D ? T_D1D : d;
         const int Q1D = T_Q1D ? T_Q1D : q;
         constexpr int MQ1 = T_Q1D ? T_Q1D : T_MAX;
         constexpr int MD1 = T_D1D ? T_D1D : T_MAX;

         MFEM_SHARED double s_BG[2][MQ1*MD1];
         MFEM_SHARED double s_DDD[3][MD1*MD1*MD1];
         MFEM_SHARED double s_DDQ[9][MD1*MD1*MQ1];
         MFEM_SHARED double s_DQQ[9][MD1*MQ1*MQ1];
         MFEM_SHARED double s_QQQ[9][MQ1*MQ1*MQ1];

         kernels::internal::LoadX<MD1>(e,D1D,X,s_DDD);
         kernels::internal::LoadBG<MD1,MQ1>(D1D,Q1D,B,G,s_BG);

         kernels::internal::GradX<MD1,MQ1>(D1D,Q1D,s_BG,s_DDD,s_DDQ);
         kernels::internal::GradY<MD1,MQ1>(D1D,Q1D,s_BG,s_DDQ,s_DQQ);
         kernels::internal::GradZ<MD1,MQ1>(D1D,Q1D,s_BG,s_DQQ,s_QQQ);

         MFEM_FOREACH_THREAD(qz,z,Q1D)
         {
            MFEM_FOREACH_THREAD(qy,y,Q1D)
            {
               MFEM_FOREACH_THREAD(qx,x,Q1D)
               {
                  const double *Jtr = &J(0,0,qx,qy,qz,e);
                  const double detJtr = kernels::Det<3>(Jtr);
                  const double weight = metric_normal * W(qx,qy,qz) * detJtr;

                  // Jrt = Jtr^{-1}
                  double Jrt[9];
                  kernels::CalcInverse<3>(Jtr, Jrt);

                  // Jpr = X^T.DSh
                  double Jpr[9];
                  kernels::internal::PullGrad<MQ1>(Q1D,qx,qy,qz,s_QQQ,Jpr);

                  // Jpt = X^T.DS = (X^T.DSh).Jrt = Jpr.Jrt
                  double Jpt[9];
                  kernels::Mult(3,3,3, Jpr, Jrt, Jpt);

                  double P[9];
                  METRIC{}.EvalP(Jpt, w, P);

                  for (int i = 0; i < 9; i++) { P[i] *= weight; }

                  // Y += DS . P^t += DSh . (Jrt . P^t)
                  double A[9];
                  kernels::MultABt(3,3,3, Jrt, P, A);
                  kernels::internal::PushGrad<MQ1>(Q1D,qx,qy,qz,A,s_QQQ);
               }
            }
         }
         MFEM_SYNC_THREAD;
         kernels::internal::LoadBGt<MD1,MQ1>(D1D,Q1D,B,G,s_BG);
         kernels::internal::GradZt<MD1,MQ1>(D1D,Q1D,s_BG,s_QQQ,s_DQQ);
         kernels::internal::GradYt<MD1,MQ1>(D1D,Q1D,s_BG,s_DQQ,s_DDQ);
         kernels::internal::GradXt<MD1,MQ1>(D1D,Q1D,s_BG,s_DDQ,Y,e);
      });
   }
};

} // namespace mfem
