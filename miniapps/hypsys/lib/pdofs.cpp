#include "pdofs.hpp"

ParDofInfo::ParDofInfo(ParFiniteElementSpace *pfes_sltn,
                       ParFiniteElementSpace *pfes_bounds, int NumEq_)
   : DofInfo(pfes_sltn, pfes_bounds, NumEq_),
     pmesh(pfes_sltn->GetParMesh()), pfes(pfes_sltn),
     px_min(pfes_bounds), px_max(pfes_bounds)
{
   FillNeighborDofs();
}

void ParDofInfo::FillNeighborDofs()
{
   // Use the first mesh element as indicator.
   const FiniteElement &el = *pfes->GetFE(0);
   int i, j, e, nbr, ne = pmesh->GetNE();
   int nd = el.GetDof(), p = el.GetOrder();
   Array <int> bdrs, orientation;
   FaceElementTransformations *Trans;

   pmesh->ExchangeFaceNbrData();
   Table *face_to_el = pmesh->GetFaceToAllElementTable();

   NbrDofs.SetSize(NumBdrs, NumFaceDofs, ne);

   // Permutations of BdrDofs, taking into account all possible orientations.
   // Assumes BdrDofs are ordered in xyz order, which is true for 3D hexes,
   // but it isn't true for 2D quads.
   int orient_cnt = 1;
   if (dim == 2) { orient_cnt = 2; }
   if (dim == 3) { orient_cnt = 8; }
   const int dof1D_cnt = p+1;
   DenseTensor fdof_ids(NumFaceDofs, NumBdrs, orient_cnt);
   for (int ori = 0; ori < orient_cnt; ori++)
   {
      for (int face_id = 0; face_id < NumBdrs; face_id++)
      {
         for (int fdof_id = 0; fdof_id < NumFaceDofs; fdof_id++)
         {
            // Index of fdof_id in the current orientation.
            const int ori_fdof_id = GetLocalFaceDofIndex(dim, face_id, ori,
                                                         fdof_id, dof1D_cnt);
            fdof_ids(ori)(ori_fdof_id, face_id) = BdrDofs(fdof_id, face_id);
         }
      }
   }

   for (e = 0; e < ne; e++)
   {
      if (dim==1)
      {
         pmesh->GetElementVertices(e, bdrs);

         for (i = 0; i < NumBdrs; i++)
         {
            const int nbr_cnt = face_to_el->RowSize(bdrs[i]);
            if (nbr_cnt == 1)
            {
               // No neighbor element.
               NbrDofs(i,0,e) = -1;
               continue;
            }

            int el1_id, el2_id, nbr_id;
            pmesh->GetFaceElements(bdrs[i], &el1_id, &el2_id);
            if (el2_id < 0)
            {
               // This element is in a different mpi task.
               el2_id = -1 - el2_id + ne;
            }
            nbr_id = (el1_id == e) ? el2_id : el1_id;
            NbrDofs(i,0,e) = nbr_id*nd + BdrDofs(0,(i+1)%2);
         }
      }
      else if (dim==2)
      {
         pmesh->GetElementEdges(e, bdrs, orientation);

         for (i = 0; i < NumBdrs; i++)
         {
            const int nbr_cnt = face_to_el->RowSize(bdrs[i]);
            if (nbr_cnt == 1)
            {
               // No neighbor element.
               for (j = 0; j < NumFaceDofs; j++) { NbrDofs(i,j,e) = -1; }
               continue;
            }

            int el1_id, el2_id, nbr_id;
            pmesh->GetFaceElements(bdrs[i], &el1_id, &el2_id);
            if (el2_id < 0)
            {
               // This element is in a different mpi task.
               el2_id = -1 - el2_id + ne;
            }
            nbr_id = (el1_id == e) ? el2_id : el1_id;

            int el1_info, el2_info;
            pmesh->GetFaceInfos(bdrs[i], &el1_info, &el2_info);
            const int face_id_nbr = (nbr_id == el1_id) ? el1_info / 64
                                    : el2_info / 64;
            for (j = 0; j < NumFaceDofs; j++)
            {
               // Here it is utilized that the orientations of the face for
               // the two elements are opposite of each other.
               NbrDofs(i,j,e) = nbr_id*nd + BdrDofs(NumFaceDofs - 1 - j,
                                                    face_id_nbr);
            }
         }
      }
      else if (dim==3)
      {
         pmesh->GetElementFaces(e, bdrs, orientation);

         for (int f = 0; f < NumBdrs; f++)
         {
            const int nbr_cnt = face_to_el->RowSize(bdrs[f]);
            if (nbr_cnt == 1)
            {
               // No neighbor element.
               for (j = 0; j < NumFaceDofs; j++) { NbrDofs(f,j,e) = -1; }
               continue;
            }

            int el1_id, el2_id, nbr_id;
            pmesh->GetFaceElements(bdrs[f], &el1_id, &el2_id);
            if (el2_id < 0)
            {
               // This element is in a different mpi task.
               el2_id = -1 - el2_id + ne;
            }
            nbr_id = (el1_id == e) ? el2_id : el1_id;

            // Local index and orientation of the face, when considered in
            // the neighbor element.
            int el1_info, el2_info;
            pmesh->GetFaceInfos(bdrs[f], &el1_info, &el2_info);
            const int face_id_nbr = (nbr_id == el1_id) ? el1_info / 64
                                    : el2_info / 64;
            const int face_or_nbr = (nbr_id == el1_id) ? el1_info % 64
                                    : el2_info % 64;
            for (j = 0; j < NumFaceDofs; j++)
            {
               // What is the index of the j-th dof on the face, given its
               // orientation.
               const int loc_face_dof_id =
                  GetLocalFaceDofIndex(dim, face_id_nbr, face_or_nbr,
                                       j, dof1D_cnt);
               // What is the corresponding local dof id on the element,
               // given the face orientation.
               const int nbr_dof_id =
                  fdof_ids(face_or_nbr)(loc_face_dof_id, face_id_nbr);

               NbrDofs(f,j,e) = nbr_id*nd + nbr_dof_id;
            }
         }
      }
   }

   for (e = 0; e < pfes->GetNBE(); e++)
   {
      const int bdr_attr = mesh->GetBdrAttribute(e);
      FaceElementTransformations *tr = mesh->GetBdrFaceTransformations(e);

      if (tr != NULL)
      {
         const int el = tr->Elem1No;

         if (dim == 1) { mesh->GetElementVertices(el, bdrs); }
         else if (dim == 2) { mesh->GetElementEdges(el, bdrs, orientation); }
         else if (dim == 3) { mesh->GetElementFaces(el, bdrs, orientation); }

         for (i = 0; i < NumBdrs; i++)
         {
            if (bdrs[i] == mesh->GetBdrElementEdgeIndex(e))
            {
               for (j = 0; j < NumFaceDofs; j++)
               {
                  NbrDofs(i, j, el) = -bdr_attr;
               }
               continue;
            }
         }
      }
      else
      {
         MFEM_ABORT("Something went wrong.");
      }
   }

   delete face_to_el;
}

void ParDofInfo::ComputeBounds(const Vector &x)
{
   ParFiniteElementSpace *pfesCG = px_min.ParFESpace();
   GroupCommunicator &gcomm = pfesCG->GroupComm();
   const int nd = pfesCG->GetFE(0)->GetDof();
   const int ne = mesh->GetNE();
   Array<int> dofsCG;

   // Form min/max at each CG dof, considering element overlaps.
   for (int n = 0; n < NumEq; n++)
   {
      x_min =  std::numeric_limits<double>::infinity();
      x_max = -std::numeric_limits<double>::infinity();

      for (int e = 0; e < ne; e++)
      {
         px_min.FESpace()->GetElementDofs(e, dofsCG);

         // These are less restrictive bounds.
         /*
         double xe_min =  std::numeric_limits<double>::infinity();
         double xe_max = -std::numeric_limits<double>::infinity();

         for (int j = 0; j < nd; j++)
         {
            xe_min = min(xe_min, x(e*nd+j));
            xe_max = max(xe_max, x(e*nd+j));
         }

         for (int j = 0; j < nd; j++)
         {
            px_min(dofsCG[j]) = min(px_min(dofsCG[j]), xe_min);
            px_max(dofsCG[j]) = max(px_max(dofsCG[j]), xe_max);
         }
         */

         // Tight bounds.
         for (int i = 0; i < nd; i++)
         {
            for (int j = 0; j < ClosestNbrs.Width(); j++)
            {
               if (ClosestNbrs(i,j) == -1) { break; }

               const int I = dofsCG[DofMapH1[i]];
               const int J = n*ne*nd + e*nd+ClosestNbrs(i,j);
               px_min(I) = min(px_min(I), x(J));
               px_max(I) = max(px_max(I), x(J));
            }
         }
      }

      Array<double> minvals(px_min.GetData(), px_min.Size()),
            maxvals(px_max.GetData(), px_max.Size());

      gcomm.Reduce<double>(minvals, GroupCommunicator::Min);
      gcomm.Bcast(minvals);
      gcomm.Reduce<double>(maxvals, GroupCommunicator::Max);
      gcomm.Bcast(maxvals);

      // Use (px_min, px_max) to fill (xi_min, xi_max) for each DG dof.
      for (int e = 0; e < ne; e++)
      {
         px_min.FESpace()->GetElementDofs(e, dofsCG);
         for (int j = 0; j < nd; j++)
         {
            xi_min(n*ne*nd + e*nd + j) = px_min(dofsCG[DofMapH1[j]]);
            xi_max(n*ne*nd + e*nd + j) = px_max(dofsCG[DofMapH1[j]]);
         }
      }
   }
}
