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

#include "../../config/config.hpp"

#ifdef MFEM_USE_MPI

#include "pncsubmesh.hpp"

#include <numeric>
#include <unordered_map>
#include "submesh_utils.hpp"
#include "psubmesh.hpp"
#include "../ncmesh_tables.hpp"

namespace mfem
{

using namespace SubMeshUtils;


ParNCSubMesh::ParNCSubMesh(ParSubMesh& submesh,
   const ParNCMesh &parent, From from, const Array<int> &attributes)
: ParNCMesh(), parent_(&parent), from_(from), attributes_(attributes)
{
   MyComm = submesh.GetComm();
   NRanks = submesh.GetNRanks();
   MyRank = submesh.GetMyRank();

   // get global rank
   int rank;
   MPI_Comm_rank(MPI_COMM_WORLD, &rank);

   Dim = submesh.Dimension();
   spaceDim = submesh.SpaceDimension();
   Iso = true;
   Legacy = false;

   UniqueIndexGenerator node_ids;
   // Loop over parent leaf elements and add nodes for all vertices. Register as top level
   // nodes, will reparent when looping over edges. Cannot add edge nodes at same time
   // because top level vertex nodes must be contiguous and first in node list (see
   // coordinates).
   if (from == From::Domain)
   {
      // Loop over elements of the parent NCMesh. If the element has the attribute, copy it.
      // parent_to_submesh_element_ids_.SetSize(parent.elements.Size());
      // parent_to_submesh_element_ids_ = -1;
      parent_to_submesh_element_ids_.reserve(parent.elements.Size());

      std::set<int> new_nodes;
      for (int ipe = 0; ipe < parent.elements.Size(); ipe++)
      {
         const auto& pe = parent.elements[ipe];
         if (!HasAttribute(pe, attributes)) { continue; }

         const int elem_id = AddElement(pe);
         NCMesh::Element &el = elements[elem_id];
         parent_element_ids_.Append(ipe); // submesh -> parent
         parent_to_submesh_element_ids_[ipe] = elem_id; // parent -> submesh
         el.index = submesh.GetSubMeshElementFromParent(el.index);
         if (!pe.IsLeaf()) { continue; }
         const auto gi = GI[pe.geom];
         bool new_id = false;
         for (int n = 0; n < gi.nv; n++)
         {
            new_nodes.insert(el.node[n]); // el.node are still from parent mesh at this stage.
         }
         for (int e = 0; e < gi.ne; e++)
         {
            new_nodes.insert(parent.nodes.FindId(el.node[gi.edges[e][0]],
                                                 el.node[gi.edges[e][1]]));
         }
      }

      parent_node_ids_.Reserve(static_cast<int>(new_nodes.size()));
      parent_to_submesh_node_ids_.reserve(new_nodes.size());
      for (const auto &n : new_nodes)
      {
         bool new_node;
         auto new_node_id = node_ids.Get(n, new_node);
         MFEM_ASSERT(new_node, "!");
         nodes.Alloc(new_node_id, new_node_id, new_node_id);

         parent_node_ids_.Append(n);
         parent_to_submesh_node_ids_[n] = new_node_id;
      }

      // Loop over submesh vertices, and add each node. Given submesh vertices respect
      // ordering of vertices in the parent mesh, this ensures all top level vertices are
      // added first as top level nodes. Some of these nodes will not be top level nodes,
      // and will require reparenting based on edge data.
      for (int iv = 0; iv < submesh.GetNV(); iv++)
      {
         bool new_node;
         int parent_vertex_id = submesh.GetParentVertexIDMap()[iv];
         int parent_node_id = parent.vertex_nodeId[parent_vertex_id];
         auto new_node_id = node_ids.Get(parent_node_id, new_node);
         MFEM_ASSERT(!new_node, "Each vertex's node should have already been added");
         nodes[new_node_id].vert_index = iv;
      }

      // Loop over elements and reference edges and faces (creating any nodes on first encounter).
      for (auto &el : elements)
      {
         if (el.IsLeaf())
         {
            const auto gi = GI[el.geom];
            bool new_id = false;

            for (int n = 0; n < gi.nv; n++)
            {
               // Relabel nodes from parent to submesh.
               el.node[n] = node_ids.Get(el.node[n], new_id);
               MFEM_ASSERT(new_id == false, "Should not be new.");
               nodes[el.node[n]].vert_refc++;
            }
            for (int e = 0; e < gi.ne; e++)
            {
               const int pid = parent.nodes.FindId(
                  parent_node_ids_[el.node[gi.edges[e][0]]],
                  parent_node_ids_[el.node[gi.edges[e][1]]]);
               MFEM_ASSERT(pid >= 0, "Edge not found");
               auto submesh_node_id = node_ids.Get(pid, new_id); // Convert parent id to a new submesh id.
               if (new_id)
               {
                  nodes.Alloc(submesh_node_id, submesh_node_id, submesh_node_id);
                  parent_node_ids_.Append(pid);
                  parent_to_submesh_node_ids_[pid] = submesh_node_id;
               }
               nodes[submesh_node_id].edge_refc++; // Register the edge
            }
            for (int f = 0; f < gi.nf; f++)
            {
               const int *fv = gi.faces[f];
               const int pid = parent.faces.FindId(
                  parent_node_ids_[el.node[fv[0]]],
                  parent_node_ids_[el.node[fv[1]]],
                  parent_node_ids_[el.node[fv[2]]],
                  el.node[fv[3]] >= 0 ? parent_node_ids_[el.node[fv[3]]]: - 1);
               MFEM_ASSERT(pid >= 0, "Face not found");
               const int id = faces.GetId(el.node[fv[0]], el.node[fv[1]], el.node[fv[2]], el.node[fv[3]]);
               // parent_face_ids_.Append(pid);
               // parent_to_submesh_face_ids_[pid] = id;
               faces[id].attribute = parent.faces[pid].attribute;
            }
         }
         else
         {
            // All elements have been collected, remap the child ids.
            for (int i = 0; i < ref_type_num_children[el.ref_type]; i++)
            {
               el.child[i] = parent_to_submesh_element_ids_[el.child[i]];
            }
         }
         el.parent = el.parent < 0 ? el.parent : parent_to_submesh_element_ids_.at(el.parent);
      }
   }
   else if (from == From::Boundary)
   {
      auto face_geom_from_nodes = [](const std::array<int, MaxFaceNodes> &nodes)
      {
         if (nodes[3] == -1){ return Geometry::Type::TRIANGLE; }
         if (nodes[0] == nodes[1] && nodes[2] == nodes[3]) { return Geometry::Type::SEGMENT; }
         return Geometry::Type::SQUARE;
      };

      // Helper struct for storing FaceNodes and allowing comparisons based on the sorted
      // set of nodes.
      struct FaceNodes
      {
         std::array<int, MaxFaceNodes> nodes;
         bool operator<(FaceNodes t2) const
         {
            std::array<int, MaxFaceNodes> t1 = nodes;
            std::sort(t1.begin(), t1.end());
            std::sort(t2.nodes.begin(), t2.nodes.end());
            return std::lexicographical_compare(t1.begin(), t1.end(),
               t2.nodes.begin(), t2.nodes.end());
         };
      };

      // Map from parent nodes to the new element in the ncsubmesh.
      std::map<FaceNodes, int> pnodes_new_elem;
      // Collect parent vertex nodes to add in sequence.
      std::set<int> new_nodes;
      parent_to_submesh_element_ids_.reserve(parent.faces.Size());
      parent_element_ids_.Reserve(parent.faces.Size());
      const auto &face_list = const_cast<std::decay<decltype(parent)>::type&>(parent).GetFaceList();
      const auto &face_to_be = submesh.GetParent()->GetFaceToBdrElMap();
      // Double indexing loop because parent.faces begin() and end() do not align with
      // index 0 and size-1.
      for (int i = 0, ipe = 0; ipe < parent.faces.Size(); i++)
      {
         const auto &face = parent.faces[i];
         if (face.Unused()) { continue; }
         ipe++; // actual possible parent element.
         const auto &elem = parent.elements[face.elem[0] >= 0 ? face.elem[0] : face.elem[1]];
         if (!HasAttribute(face, attributes)
         || face_list.GetMeshIdType(face.index) == NCList::MeshIdType::MASTER
         ) { continue; }

         auto face_type = face_list.GetMeshIdType(face.index);
         auto fn = FaceNodes{parent.FindFaceNodes(face)};
         if (pnodes_new_elem.find(fn) != pnodes_new_elem.end()){ continue; }

         // TODO: Internal nc submesh can be constructed and solved on, but the transfer
         // to the parent mesh can be erroneous, this is likely due to not treating the
         // changing orientation of internal faces for ncmesh within the ptransfermap.
         MFEM_ASSERT(face.elem[0] < 0 || face.elem[1] < 0,
            "Internal nonconforming boundaries are not reliably supported yet.");

         auto face_geom = face_geom_from_nodes(fn.nodes);
         int new_elem_id = AddElement(face_geom, face.attribute);

         // Rank needs to be established by presence (or lack of) in the submesh.
         elements[new_elem_id].rank = [&parent, &face, &submesh, &face_to_be]()
         {
            auto rank0 = face.elem[0] >= 0 ? parent.elements[face.elem[0]].rank : -1;
            auto rank1 = face.elem[1] >= 0 ? parent.elements[face.elem[1]].rank : -1;

            if (rank0 < 0) { return rank1; }
            if (rank1 < 0) { return rank0; }

            // A left and right element are present, need to establish which side
            // received the boundary element.
            return rank0 < rank1 ? rank0 : rank1;
         }();

         pnodes_new_elem[fn] = new_elem_id;
         parent_element_ids_.Append(i);
         parent_to_submesh_element_ids_[i] = new_elem_id;


         // Copy in the parent nodes. These will be relabeled once the tree is built.
         std::copy(fn.nodes.begin(), fn.nodes.end(), elements[new_elem_id].node);
         for (auto x : fn.nodes)
            if (x != -1)
            {
               new_nodes.insert(x);
            }
         auto &gi = GI[face_geom];
         gi.InitGeom(face_geom);
         for (int e = 0; e < gi.ne; e++)
         {
            new_nodes.insert(parent.nodes.FindId(fn.nodes[gi.edges[e][0]],
                                                   fn.nodes[gi.edges[e][1]]));
         }

         /*
            - Check not top level face
            - Check for parent of the newly entered element
               - if not present, add in
               - if present but different order, reorder so consistent with child
                  elements.
            - Set .child in the parent of the newly entered element
            - Set .parent in the newly entered element

            Break if top level face or joined existing branch (without reordering).
         */
         while (true)
         {
            int child = parent.ParentFaceNodes(fn.nodes);
            if (child == -1) // A root face
            {
               elements[new_elem_id].parent = -1;
               break;
            }

            auto pelem = pnodes_new_elem.find(fn);
            bool new_parent = pelem == pnodes_new_elem.end();
            bool fix_parent = false;
            if (new_parent)
            {
               // Add in this parent
               int pelem_id = AddElement(NCMesh::Element(face_geom_from_nodes(fn.nodes), face.attribute));
               pelem = pnodes_new_elem.emplace(fn, pelem_id).first;
               auto parent_face_id = parent.faces.FindId(fn.nodes[0], fn.nodes[1], fn.nodes[2], fn.nodes[3]);
               parent_element_ids_.Append(parent_face_id);
            }
            else
            {
               // There are two scenarios where the parent nodes should be rearranged:
               // 1. The found face is a slave, then the master might have been added in
               //    reverse orientation
               // 2. The parent face was added from the central face of a triangle, the
               //    orientation of the parent face is only fixed relative to the outer
               //    child faces not the interior.
               //    If either of these scenarios, and there's a mismatch, then reorder
               //    the parent and all ancestors if necessary.
               if (((elem.Geom() == Geometry::Type::TRIANGLE && child != 3) || face_type != NCList::MeshIdType::UNRECOGNIZED)
               && !std::equal(fn.nodes.begin(), fn.nodes.end(), pelem->first.nodes.begin()))
               {
                  fix_parent = true;
                  auto pelem_id = pelem->second;
                  auto &parent_elem = elements[pelem->second];
                  if (parent_elem.IsLeaf())
                  {
                     // Set all node to -1. Check that they are all filled appropriately.
                     for (int n = 0; n < MaxElemNodes; n++)
                     {
                        elements[pelem->second].node[n] = -1;
                     }
                  }
                  else
                  {
                     // This face already had children, reorder them to match the
                     // permutation from the original nodes to the new face nodes. The
                     // discovered parent order should be the same for all descendent
                     // faces, if this branch is triggered twice for a given parent face,
                     // duplicate child elements will be marked.
                     int child[MaxFaceNodes];
                     for (int i1 = 0; i1 < MaxFaceNodes; i1++)
                        for (int i2 = 0; i2 < MaxFaceNodes; i2++)
                           if (fn.nodes[i1] == pelem->first.nodes[i2])
                           {
                              child[i2] = parent_elem.child[i1]; break;
                           }
                     std::copy(child, child+MaxFaceNodes, parent_elem.child);
                  }
                  // Re-key the map
                  pnodes_new_elem.erase(pelem->first);
                  pelem = pnodes_new_elem.emplace(fn, pelem_id).first;
               }
            }
            // Ensure parent element is marked as non-leaf.
            elements[pelem->second].ref_type = Dim == 2 ? Refinement::XY : Refinement::X;
            // Know that the parent element exists, connect parent and child
            elements[pelem->second].child[child] = new_elem_id;
            elements[new_elem_id].parent = pelem->second;

            // If this was neither new nor a fixed parent, the higher levels of the tree have been built,
            // otherwise we recurse up the tree to add/fix more parents.
            if (!new_parent && !fix_parent) { break; }
            new_elem_id = pelem->second;
         }
      }
      parent_element_ids_.ShrinkToFit();

      MFEM_ASSERT(parent_element_ids_.Size() == elements.Size(), parent_element_ids_.Size() << ' ' << elements.Size());
      std::vector<FaceNodes> new_elem_to_parent_face_nodes(pnodes_new_elem.size());
      /*
         All elements have been added into the tree but
         a) The nodes are all from the parent ncmesh
         b) The nodes do not know their parents
         c) The element ordering is wrong, root elements are not first
         d) The parent and child element numbers reflect the incorrect ordering

         1. Add in nodes in the same order from the parent ncmesh
         2. Compute reordering of elements with root elements first.
      */

      // Add new nodes preserving parent mesh ordering
      parent_node_ids_.Reserve(static_cast<int>(new_nodes.size()));
      parent_to_submesh_node_ids_.reserve(new_nodes.size());
      for (auto n : new_nodes)
      {
         bool new_node;
         auto new_node_id = node_ids.Get(n, new_node);
         MFEM_ASSERT(new_node, "!");
         nodes.Alloc(new_node_id, new_node_id, new_node_id);
         parent_node_ids_.Append(n);
         parent_to_submesh_node_ids_[n] = new_node_id;
      }
      parent_node_ids_.ShrinkToFit();
      new_nodes.clear(); // not needed any more.

      // Comparator for deciding order of elements. Building the ordering from the parent
      // ncmesh ensures the root ordering is common across ranks.
      auto comp_elements = [&](int l, int r)
      {
         const auto &elem_l = elements[l];
         const auto &elem_r = elements[r];
         if (elem_l.parent == elem_r.parent)
         {
            const auto &fnl = new_elem_to_parent_face_nodes.at(l).nodes;
            const auto &fnr = new_elem_to_parent_face_nodes.at(r).nodes;
            return std::lexicographical_compare(fnl.begin(), fnl.end(), fnr.begin(), fnr.end());
         }
         else
         {
            return elem_l.parent < elem_r.parent;
         }
      };

      auto parental_sorted = [&](const BlockArray<Element> &elements)
      {
         Array<int> indices(elements.Size());
         std::iota(indices.begin(), indices.end(), 0);

         return std::is_sorted(indices.begin(), indices.end(), comp_elements);
      };

      auto print_elements = [&](bool parent_nodes = true){
         for (int e = 0; e < elements.Size(); e++)
         {
            auto &elem = elements[e];
               auto &gi = GI[elem.geom];
               gi.InitGeom(elem.Geom());
            std::cout << "element " << e
            << " elem.attribute " << elem.attribute;

            if (elem.IsLeaf())
            {
               std::cout << " node ";
               for (int n = 0; n < gi.nv; n++)
               {
                  std::cout << (parent_nodes ? parent_to_submesh_node_ids_[elem.node[n]] : elem.node[n]) << ' ';
               }
            }
            else
            {
               std::cout << " child ";
               for (int c = 0; c < MaxElemChildren && elem.child[c] >= 0; c++)
               {
                  std::cout << elem.child[c] << ' ';
               }
            }

            std::cout << "parent " << elem.parent << '\n';
         }
         std::cout << "new_elem_to_parent_face_nodes\n";
         for (int i = 0; i < new_elem_to_parent_face_nodes.size(); i++)
         {
            const auto &n = new_elem_to_parent_face_nodes[i].nodes;
            std::cout << i << ": " << n[0] << ' ' << n[1] << ' ' << n[2] << ' ' << n[3] << '\n';
         }
      };

      Array<int> new_to_old(elements.Size()), old_to_new(elements.Size());
      int sorts = 0;
      while (!parental_sorted(elements))
      {
         // Stably reorder elements in order of refinement, and by parental nodes within
         // a nuclear family.
         new_to_old.SetSize(elements.Size()), old_to_new.SetSize(elements.Size());
         std::iota(new_to_old.begin(), new_to_old.end(), 0);
         std::stable_sort(new_to_old.begin(), new_to_old.end(), comp_elements);

         // Build the inverse relation -> for converting the old elements to new
         for (int i = 0; i < elements.Size(); i++)
         {
            old_to_new[new_to_old[i]] = i;
         }

         // Permute whilst reordering new_to_old. Avoids unnecessary copies.
         Permute(std::move(new_to_old), elements, parent_element_ids_, new_elem_to_parent_face_nodes);

         parent_to_submesh_element_ids_.clear();
         for (int i = 0; i < parent_element_ids_.Size(); i++)
         {
            if (parent_element_ids_[i] == -1) {continue;}
            parent_to_submesh_element_ids_[parent_element_ids_[i]] = i;
         }

         // Apply the new ordering to child and parent elements
         for (auto &elem : elements)
         {
            if (!elem.IsLeaf())
            {
               // Parent rank is minimum of child ranks.
               elem.rank = std::numeric_limits<int>::max();
               for (int c = 0; c < MaxElemChildren && elem.child[c] >= 0; c++)
               {
                  elem.child[c] = old_to_new[elem.child[c]];
                  elem.rank = std::min(elem.rank, elements[elem.child[c]].rank);
               }
            }
            elem.parent = elem.parent == -1 ? -1 : old_to_new[elem.parent];
         }
      }

      // Apply new node ordering to relations, and sign in on edges/vertices
      for (auto &elem : elements)
      {
         if (elem.IsLeaf())
         {
            bool new_id;
            auto &gi = GI[elem.geom];
            gi.InitGeom(elem.Geom());
            for (int e = 0; e < gi.ne; e++)
            {
               const int pid = parent.nodes.FindId(
                  elem.node[gi.edges[e][0]], elem.node[gi.edges[e][1]]);
               MFEM_ASSERT(pid >= 0, elem.node[gi.edges[e][0]] << ' ' << elem.node[gi.edges[e][1]]);
               auto submesh_node_id = node_ids.Get(pid, new_id);
               MFEM_ASSERT(!new_id, "!");
               nodes[submesh_node_id].edge_refc++;
            }
            for (int n = 0; n < gi.nv; n++)
            {
               MFEM_ASSERT(parent_to_submesh_node_ids_.find(elem.node[n]) != parent_to_submesh_node_ids_.end(), "!");
               elem.node[n] = parent_to_submesh_node_ids_[elem.node[n]];
               nodes[elem.node[n]].vert_refc++;
            }
            // Register faces
            for (int f = 0; f < gi.nf; f++)
            {
               auto *face = faces.Get(
                  elem.node[gi.faces[f][0]],
                  elem.node[gi.faces[f][1]],
                  elem.node[gi.faces[f][2]],
                  elem.node[gi.faces[f][3]]);
               face->attribute = -1;
               face->index = -1;
            }
         }
      }
   }

   // Loop over all nodes, and reparent based on the node relations of the parent
   for (int i = 0; i < parent_node_ids_.Size(); i++)
   {
      const auto &parent_node = parent.nodes[parent_node_ids_[i]];
      const int submesh_p1 = parent_to_submesh_node_ids_[parent_node.p1];
      const int submesh_p2 = parent_to_submesh_node_ids_[parent_node.p2];
      nodes.Reparent(i, submesh_p1, submesh_p2);
   }

   nodes.UpdateUnused();
   for (int i = 0; i < elements.Size(); i++)
   {
      if (elements[i].IsLeaf())
      {
         // Register all faces
         RegisterFaces(i);
      }
   }

   InitRootElements();
   InitRootState(root_state.Size());
   InitGeomFlags();

#ifdef MFEM_DEBUG
   // Check all processors have the same number of roots
   {
      int p[2] = {root_state.Size(), -root_state.Size()};
      MPI_Allreduce(MPI_IN_PLACE, p, 2, MPI_INT, MPI_MIN, submesh.GetComm());
      MFEM_ASSERT(p[0] == -p[1], "Ranks must agree on number of root elements: min "
         << p[0] << " max " << -p[1] << " local " << root_state.Size() << " MyRank " << submesh.GetMyRank());
   }
#endif

   Update(); // Fills in secondary information based off of elements, nodes and faces.

   // If parent has coordinates defined, copy the relevant portion
   if (parent.coordinates.Size() > 0)
   {
      // Loop over new_nodes -> coordinates is indexed by node.
      coordinates.SetSize(3*parent_node_ids_.Size());
      parent.tmp_vertex = new TmpVertex[parent.nodes.NumIds()];
      for (auto pn : parent_node_ids_)
      {
         bool new_node = false;
         auto n = node_ids.Get(pn, new_node);
         MFEM_ASSERT(!new_node, "Should not be new");
         std::memcpy(&coordinates[3*n], parent.CalcVertexPos(pn), 3*sizeof(real_t));
      }
   }

   // The element indexing was changed as part of generation of leaf elements. We need to
   // update the map.
   if (from == From::Domain)
   {
      // The element indexing was changed as part of generation of leaf elements. We need to
      // update the map.
      submesh.parent_to_submesh_element_ids_ = -1;
      for (int i = 0; i < submesh.parent_element_ids_.Size(); i++)
      {
         submesh.parent_element_ids_[i] =
            parent.elements[parent_element_ids_[leaf_elements[i]]].index;
         submesh.parent_to_submesh_element_ids_[submesh.parent_element_ids_[i]] = i;
      }
   }
   else
   {
      submesh.parent_to_submesh_element_ids_ = -1;
      // parent elements are BOUNDARY elements, need to map face index to be.
      const auto &parent_face_to_be = submesh.GetParent()->GetFaceToBdrElMap();

      MFEM_ASSERT(NElements == submesh.GetNE(), "!");

      auto new_parent_to_submesh_element_ids = submesh.parent_to_submesh_element_ids_;
      Array<int> new_parent_element_ids;
      new_parent_element_ids.Reserve(submesh.parent_element_ids_.Size());
      for (int i = 0; i < submesh.parent_element_ids_.Size(); i++)
      {
         auto leaf = leaf_elements[i];
         auto pe = parent_element_ids_[leaf];
         auto pfi = parent.faces[pe].index;
         auto pbe = parent_face_to_be[pfi];
         new_parent_element_ids.Append(
            parent_face_to_be[parent.faces[parent_element_ids_[leaf_elements[i]]].index]);
         new_parent_to_submesh_element_ids[new_parent_element_ids[i]] = i;
      }

      MFEM_ASSERT(new_parent_element_ids.Size() == submesh.parent_element_ids_.Size(), "!");
#ifdef MFEM_DEBUG
      for (auto x : new_parent_element_ids)
      {
         MFEM_ASSERT(std::find(submesh.parent_element_ids_.begin(),
                               submesh.parent_element_ids_.end(), x)
                     != submesh.parent_element_ids_.end(), x << " not found in submesh.parent_element_ids_");
      }
      for (auto x : submesh.parent_element_ids_)
      {
         MFEM_ASSERT(std::find(new_parent_element_ids.begin(),
                               new_parent_element_ids.end(), x)
                     != new_parent_element_ids.end(), x << " not found in new_parent_element_ids_");
      }
#endif
      submesh.parent_element_ids_ = new_parent_element_ids;
      submesh.parent_to_submesh_element_ids_ = new_parent_to_submesh_element_ids;
   }
}

} // namespace mfem

#endif // MFEM_USE_MPI