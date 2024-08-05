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

#include "../tmop.hpp"
#include "tmop_pa.hpp"
#include "../linearform.hpp"
#include "../../general/forall.hpp"
#include "../../linalg/kernels.hpp"
#include "../../linalg/dinvariants.hpp"

namespace mfem
{

MFEM_REGISTER_TMOP_KERNELS(void, SetupGradPA_Fit_2D,
                           const int NE,
                           const real_t &c1_,
                           const real_t &c2_,
                           const Vector &x1_,
                           const Vector &x2_,
                           const Vector &x3_,
                           const Vector &x4_,
                           const Vector &x5_,
                           Vector &h0_,
                           const int d1d,
                           const int q1d)
{
    constexpr int DIM = 2;
    constexpr int NBZ = 1;

    const int D1D = T_D1D ? T_D1D : d1d;
    const int Q1D = T_Q1D ? T_Q1D : q1d;

    const auto C1 = c1_;
    const auto C2 = c2_;
    const auto X1 = Reshape(x1_.Read(), D1D, D1D, NE);
    const auto X2 = Reshape(x2_.Read(), D1D, D1D, NE);
    const auto X3 = Reshape(x3_.Read(), D1D, D1D, NE);
    const auto X4 = Reshape(x4_.Read(), D1D, D1D, DIM, NE);
    const auto X5 = Reshape(x5_.Read(), D1D, D1D, DIM, DIM, NE);



    auto H0 = Reshape(h0_.Write(), DIM, DIM, D1D, D1D, NE);

    mfem::forall_2D_batch(NE, D1D, D1D, NBZ, [=] MFEM_HOST_DEVICE (int e)
    {
        const int D1D = T_D1D ? T_D1D : d1d;
        const int Q1D = T_Q1D ? T_Q1D : q1d;
        constexpr int NBZ = 1;

        MFEM_FOREACH_THREAD(qy,y,D1D)
        {
            MFEM_FOREACH_THREAD(qx,x,D1D)
            {
            const real_t sigma = X1(qx,qy,e);
            const real_t dof_count = X2(qx,qy,e);
            const real_t marker = X3(qx,qy,e);
            const real_t coeff = C1;
            const real_t normal = C2;

            
            double w = marker * normal * coeff * 1.0/dof_count;
            for (int i = 0; i < DIM; i++)
            {
                for (int j = 0; j < DIM; j++)
                {
                    const real_t dxi = X4(qx,qy,i,e);
                    const real_t dxj = X4(qx,qy,j,e);
                    const real_t d2x = X5(qx,qy,i,j,e);
                    
                    H0(i,j,qx,qy,e) += 2 * w * sigma * (dxi*dxj + d2x);
                    
                }
            }
            
            }
        }
        MFEM_SYNC_THREAD;
    });

}
void TMOP_Integrator::AssembleGradPA_Fit_2D(const Vector &X) const
{
   const int N = PA.ne;
   const int meshOrder = surf_fit_gf->FESpace()->GetMaxElementOrder();
   const int D1D = meshOrder + 1;
   const int Q1D = D1D;
   const int id = (D1D << 4 ) | Q1D;

   const real_t &C1 = PA.C1;
   const real_t &C2 = PA.C2;
   const Vector &X1 = PA.X1;
   const Vector &X2 = PA.X2;
   const Vector &X3 = PA.X3;
   const Vector &X4 = PA.X4;
   const Vector &X5 = PA.X5;

   Vector &H0 = PA.H0Fit;

   MFEM_LAUNCH_TMOP_KERNEL(SetupGradPA_Fit_2D,id,N,C1,C2,X1,X2,X3,X4,X5,H0);
}

} // namespace mfem