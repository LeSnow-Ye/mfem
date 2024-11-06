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

#ifndef MFEM_HANDLE_HPP
#define MFEM_HANDLE_HPP

#include "../config/config.hpp"
#include "operator.hpp"
#ifdef MFEM_USE_MPI
#include "hypre.hpp"
#endif

namespace mfem
{

/// Pointer to an object
/** A general template wrapping the pointer to an object
    and optionally taking its ownership. */
template<class T>
class Handle
{
   T *ptr{};
   bool own_ptr{};

public:
   /// Create a Handle
   Handle() { }

   /// Create a Handle for the given pointer, @a A.
   /** The object ownership flag is set to the value of @a own_A.
       It is expected that @a A points to a valid object. */
   explicit Handle(T *A, bool own_A = true) { Reset(A, own_A); }

   /// Shallow copy. The ownership flag of the target is set to false.
   Handle(const Handle &other)
      : ptr(other.ptr), own_ptr(false) {  }

   virtual ~Handle() { if (own_ptr) { delete ptr; } }

   /// Shallow copy. The ownership flag of the target is set to false.
   Handle &operator=(const Handle &master)
   {
      Reset(master.Ptr(), false);
      return *this;
   }

   /// Access the underlying object pointer.
   inline T *Ptr() const { return ptr; }

   /// Support the use of -> to call methods of the underlying object.
   inline T *operator->() const { return ptr; }

   /// Access the underlying object.
   inline T &operator*() { return *ptr; }

   /// Access the underlying object.
   const T &operator*() const { return *ptr; }

   /** @brief Return the pointer statically cast to a specified PtrType.
       Similar to the method Get(). */
   template <typename PtrType>
   PtrType *As() const { return static_cast<PtrType*>(ptr); }

   /// Return the pointer dynamically cast to a specified PtrType.
   template <typename PtrType>
   PtrType *Is() const { return dynamic_cast<PtrType*>(ptr); }

   /// Return the pointer statically cast to a given PtrType.
   /** Similar to the method As(), however the template type PtrType can be
       derived automatically from the argument @a A. */
   template <typename PtrType>
   void Get(PtrType *&A) const { A = static_cast<PtrType*>(ptr); }

   /// Return true if the Handle owns the held object.
   bool Owns() const { return own_ptr; }

   /// Set the ownership flag for the held object.
   void SetOwner(bool own = true) { own_ptr = own; }

   /// Clear the Handle, deleting the held object (if owned)
   void Clear() { Reset(nullptr, false); }

   /// Reset the Handle to the given pointer, @a A.
   /** The object ownership flag is set to the value of @a own_A.
       It is expected that @a A points to a valid object. */
   void Reset(T *A, bool own_A = true)
   {
      if (own_ptr) { delete ptr; }
      ptr = A;
      own_ptr = own_A;
   }
};

/// Pointer to an Operator of a specified type
/** This class provides a common interface for global, matrix-type operators to
    be used in bilinear forms, gradients of nonlinear forms, static condensation,
    hybridization, etc. The following backends are currently supported:
      - HYPRE parallel sparse matrix (Hypre_ParCSR)
      - PETSC globally assembled parallel sparse matrix (PETSC_MATAIJ)
      - PETSC parallel matrix assembled on each processor (PETSC_MATIS)
    See also Operator::Type.
*/
class OperatorHandle : public Handle<Operator>
{
protected:
   static const char not_supported_msg[];

   Operator::Type type_id;

   Operator::Type CheckType(Operator::Type tid);

public:
   /** @brief Create an OperatorHandle with type id = Operator::MFEM_SPARSEMAT
       without allocating the actual matrix. */
   OperatorHandle() : type_id(Operator::MFEM_SPARSEMAT) { }

   /** @brief Create a OperatorHandle with a specified type id, @a tid, without
       allocating the actual matrix. */
   explicit OperatorHandle(Operator::Type tid) : type_id(CheckType(tid)) { }

   /// Create an OperatorHandle for the given OpType pointer, @a A.
   /** Presently, OpType can be SparseMatrix, HypreParMatrix, or PetscParMatrix.

       The operator ownership flag is set to the value of @a own_A.

       It is expected that @a A points to a valid object. */
   template <typename OpType>
   explicit OperatorHandle(OpType *A, bool own_A = true) { Reset(A, own_A); }

   /// Shallow copy. The ownership flag of the target is set to false.
   OperatorHandle(const OperatorHandle &other)
      : Handle(other), type_id(other.type_id) {  }

   ~OperatorHandle() { }

   /// Shallow copy. The ownership flag of the target is set to false.
   OperatorHandle &operator=(const OperatorHandle &master)
   {
      Handle::operator=(master);
      type_id = master.type_id;
      return *this;
   }

   /// Get the currently set operator type id.
   Operator::Type Type() const { return type_id; }

   /// Return true if the OperatorHandle owns the held Operator.
   bool OwnsOperator() const { return Owns(); }

   /// Set the ownership flag for the held Operator.
   void SetOperatorOwner(bool own = true) { SetOwner(own); }

   /// Invoke Clear() and set a new type id.
   void SetType(Operator::Type tid)
   {
      Clear();
      type_id = CheckType(tid);
   }

   /// Reset the OperatorHandle to the given OpType pointer, @a A.
   /** The Operator ownership flag is set to the value of @a own_A.
       It is expected that @a A points to a valid object. */
   template <typename OpType>
   void Reset(OpType *A, bool own_A = true)
   {
      Handle::Reset(A, own_A);
      type_id = A->GetType();
   }

#ifdef MFEM_USE_MPI
   /** @brief Reset the OperatorHandle to hold a parallel square block-diagonal
       matrix using the currently set type id. */
   /** The operator ownership flag is set to true. */
   void MakeSquareBlockDiag(MPI_Comm comm, HYPRE_BigInt glob_size,
                            HYPRE_BigInt *row_starts, SparseMatrix *diag);

   /** @brief Reset the OperatorHandle to hold a parallel rectangular
       block-diagonal matrix using the currently set type id. */
   /** The operator ownership flag is set to true. */
   void MakeRectangularBlockDiag(MPI_Comm comm, HYPRE_BigInt glob_num_rows,
                                 HYPRE_BigInt glob_num_cols, HYPRE_BigInt *row_starts,
                                 HYPRE_BigInt *col_starts, SparseMatrix *diag);
#endif // MFEM_USE_MPI

   /// Reset the OperatorHandle to hold the product @a P^t @a A @a P.
   /** The type id of the result is determined by that of @a A and @a P. The
       operator ownership flag is set to true. */
   void MakePtAP(OperatorHandle &A, OperatorHandle &P);

   /** @brief Reset the OperatorHandle to hold the product R @a A @a P, where
       R = @a Rt^t. */
   /** The type id of the result is determined by that of @a Rt, @a A, and @a P.
       The operator ownership flag is set to true. */
   void MakeRAP(OperatorHandle &Rt, OperatorHandle &A, OperatorHandle &P);

   /// Convert the given OperatorHandle @a A to the currently set type id.
   /** The operator ownership flag is set to false if the object held by @a A
       will be held by this object as well, e.g. when the source and destination
       types are the same; otherwise it is set to true. */
   void ConvertFrom(OperatorHandle &A);

   /// Convert the given OpType pointer, @a A, to the currently set type id.
   /** This method creates a temporary OperatorHandle for @a A and invokes
       ConvertFrom(OperatorHandle &) with it. */
   template <typename OpType>
   void ConvertFrom(OpType *A)
   {
      OperatorHandle Ah(A, false);
      ConvertFrom(Ah);
   }

   /** @brief Reset the OperatorHandle to be the eliminated part of @a A after
       elimination of the essential dofs @a ess_dof_list. */
   void EliminateRowsCols(OperatorHandle &A, const Array<int> &ess_dof_list);

   /// Eliminate the rows corresponding to the essential dofs @a ess_dof_list
   void EliminateRows(const Array<int> &ess_dof_list);

   /// Eliminate columns corresponding to the essential dofs @a ess_dof_list
   void EliminateCols(const Array<int> &ess_dof_list);

   /// Eliminate essential dofs from the solution @a X into the r.h.s. @a B.
   /** The argument @a A_e is expected to be the result of the method
       EliminateRowsCols(). */
   void EliminateBC(const OperatorHandle &A_e, const Array<int> &ess_dof_list,
                    const Vector &X, Vector &B) const;
};


/// Add an alternative name for OperatorHandle -- OperatorPtr.
typedef OperatorHandle OperatorPtr;

} // namespace mfem

#endif
