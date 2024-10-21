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

#include "../tmop_pa.hpp"
#include "../tmop_pa_p2.hpp"

namespace mfem
{

void TMOPAddMultPA_001(TMOPAddMultPA2D &ker)
{
   TMOPKernelLaunch<TMOP_PA_Metric_001>(ker);
}

} // namespace mfem
