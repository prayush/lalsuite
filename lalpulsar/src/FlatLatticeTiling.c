//
// Copyright (C) 2007, 2008, 2012, 2014 Karl Wette
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with with program; see the file COPYING. If not, write to the
// Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
// MA  02111-1307  USA
//

#include <config.h>
#include <sys/types.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <gsl/gsl_permutation.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_eigen.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_nan.h>
#include <gsl/gsl_sf.h>

#include <lal/FlatLatticeTiling.h>
#include <lal/LALStdlib.h>
#include <lal/LALMalloc.h>
#include <lal/LALConstants.h>
#include <lal/XLALError.h>
#include <lal/GSLSupport.h>

#ifdef __GNUC__
#define UNUSED __attribute__ ((unused))
#else
#define UNUSED
#endif

///
/// Flat lattice tiling bound info
///
typedef struct tagFLT_Bound {
  bool tiled;                                   ///< Is the bound tiled, i.e. non-singular?
  FlatLatticeBound func;                        ///< Parameter space bound function
  void* data;                                   ///< Arbitrary data describing parameter space
} FLT_Bound;

///
/// Flat lattice tiling status
///
typedef enum tagFLT_Status {
  FLT_S_INCOMPLETE,
  FLT_S_INITIALISED,
  FLT_S_STARTED,
  FLT_S_FINISHED
} FLT_Status;

///
/// Maximum number of parameter space bounds per dimension
///
#define MAX_BOUNDS 4

///
/// Flat lattice tiling state structure
///
struct tagFlatLatticeTiling {
  size_t dimensions;                            ///< Dimension of the parameter space
  size_t tiled_dimensions;                      ///< Tiled dimension of the parameter space
  FLT_Status status;                            ///< Status of the tiling
  FLT_Bound *bounds;                            ///< Array of parameter space bound info for each dimension
  FlatLatticeGenerator generator;               ///< Flat tiling lattice generator function
  gsl_vector* phys_scale;                       ///< Normalised to physical coordinate scaling
  gsl_vector* phys_offset;                      ///< Normalised to physical coordinate offset
  gsl_vector* phys_incr;                        ///< Physical increments of the lattice tiling generator
  gsl_vector* phys_bbox;                        ///< Physical metric ellipse bounding box extents
  gsl_matrix* metric;                           ///< Normalised parameter space metric
  gsl_matrix* increment;                        ///< Increment vectors of the lattice tiling generator
  gsl_vector* curr_point;                       ///< Current lattice point
  gsl_vector_uint* curr_bound;                  ///< Indices of current bound on parameter space
  gsl_matrix* curr_lower;                       ///< Current lower bound on parameter space
  gsl_matrix* curr_upper;                       ///< Current upper bound on parameter space
  gsl_vector* curr_lower_pad;                   ///< Current lower padding of parameter space
  gsl_vector* curr_upper_pad;                   ///< Current upper padding of parameter space
  gsl_vector* curr_phys_point;                  ///< Current physical parameter-space point
  unsigned long count;                          ///< Total number of points generated so far
};

///
/// Find the bounding box of the mismatch ellipses of a metric
///
static gsl_vector* FLT_MetricEllipseBoundingBox(
  const gsl_matrix* metric,                     ///< [in] Metric to bound
  const double max_mismatch                     ///< [in] Maximum mismatch with respect to metric
  )
{

  const size_t n = metric->size1;

  // Check input
  XLAL_CHECK_NULL(metric->size1 == metric->size2, XLAL_ESIZE);

  // Allocate memory
  gsl_matrix* LU_decomp = gsl_matrix_alloc(n, n);
  XLAL_CHECK_NULL(LU_decomp != NULL, XLAL_ENOMEM);
  gsl_permutation* LU_perm = gsl_permutation_alloc(n);
  XLAL_CHECK_NULL(LU_perm != NULL, XLAL_ENOMEM);
  gsl_matrix* inverse = gsl_matrix_alloc(n, n);
  XLAL_CHECK_NULL(inverse != NULL, XLAL_ENOMEM);
  gsl_vector* bound_box = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(bound_box != NULL, XLAL_ENOMEM);

  // Compute metric inverse
  int LU_sign = 0;
  gsl_matrix_memcpy(LU_decomp, metric);
  gsl_linalg_LU_decomp(LU_decomp, LU_perm, &LU_sign);
  gsl_linalg_LU_invert(LU_decomp, LU_perm, inverse);

  // Compute bounding box
  for (size_t i = 0; i < n; ++i) {
    gsl_vector_set(bound_box, i, sqrt(max_mismatch * gsl_matrix_get(inverse, i ,i)));
  }

  // Cleanup
  gsl_matrix_free(LU_decomp);
  gsl_permutation_free(LU_perm);
  gsl_matrix_free(inverse);

  return bound_box;

}

///
/// Orthonormalise the columns of a matrix with respect to a metric (matrix is lower triangular)
///
static int FLT_OrthonormaliseWRTMetric(
  gsl_matrix* matrix,                           ///< [in] Matrix of columns to orthonormalise
  const gsl_matrix* metric                      ///< [in] Metric to orthonormalise with respect to
  )
{

  // Check input
  XLAL_CHECK(matrix != NULL, XLAL_EFAULT);
  XLAL_CHECK(metric != NULL, XLAL_EFAULT);
  XLAL_CHECK(metric->size1 == metric->size2, XLAL_ESIZE);
  XLAL_CHECK(metric->size1 == matrix->size2 && metric->size2 == matrix->size2, XLAL_ESIZE);
  const size_t n = metric->size1;

  // Allocate
  gsl_vector* temp = gsl_vector_alloc(n);
  XLAL_CHECK(temp != NULL, XLAL_ENOMEM);

  // Orthonormalise the columns of the matrix using numerically stabilised Gram-Schmidt
  for (ssize_t i = n - 1; i >= 0; --i) {
    gsl_vector_view col_i = gsl_matrix_column(matrix, i);

    double inner_prod = 0.0;

    for (ssize_t j = n - 1; j > i; --j) {
      gsl_vector_view col_j = gsl_matrix_column(matrix, j);

      // Compute inner product of jth and ith columns with the metric
      gsl_blas_dgemv(CblasNoTrans, 1.0, metric, &col_j.vector, 0.0, temp);
      gsl_blas_ddot(&col_i.vector, temp, &inner_prod);

      // Subtract component of jth column from ith column
      gsl_vector_memcpy(temp, &col_j.vector);
      gsl_vector_scale(temp, inner_prod);
      gsl_vector_sub(&col_i.vector, temp);

    }

    // Compute inner product of ith column with itself
    gsl_blas_dgemv(CblasNoTrans, 1.0, metric, &col_i.vector, 0.0, temp);
    gsl_blas_ddot(&col_i.vector, temp, &inner_prod);

    // Normalise ith column
    gsl_vector_scale(&col_i.vector, 1.0 / sqrt(inner_prod));

  }

  // Cleanup
  gsl_vector_free(temp);

  return XLAL_SUCCESS;

}

///
/// Transform a lattice generator to a square lower triangular form
///
static gsl_matrix* FLT_SquareLowerTriangularLatticeGenerator(
  gsl_matrix* generator                         ///< [in] Generator matrix of lattice
  )
{

  // Check input
  XLAL_CHECK_NULL(generator != NULL, XLAL_EFAULT);
  const size_t m = generator->size1;
  const size_t n = generator->size2;
  XLAL_CHECK_NULL(m >= n, XLAL_ESIZE);

  // Allocate memory
  gsl_matrix* QR_decomp = gsl_matrix_alloc(m, n);
  XLAL_CHECK_NULL(QR_decomp != NULL, XLAL_ENOMEM);
  gsl_vector* QR_tau = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(QR_tau != NULL, XLAL_ENOMEM);
  gsl_matrix* Q = gsl_matrix_alloc(m, m);
  XLAL_CHECK_NULL(Q != NULL, XLAL_ENOMEM);
  gsl_matrix* R = gsl_matrix_alloc(m, n);
  XLAL_CHECK_NULL(R != NULL, XLAL_ENOMEM);
  gsl_matrix* perm_sign = gsl_matrix_alloc(n, m);
  XLAL_CHECK_NULL(perm_sign != NULL, XLAL_ENOMEM);
  gsl_matrix* left = gsl_matrix_alloc(n, m);
  XLAL_CHECK_NULL(left != NULL, XLAL_ENOMEM);
  gsl_matrix* right = gsl_matrix_alloc(n, n);
  XLAL_CHECK_NULL(right != NULL, XLAL_ENOMEM);
  gsl_matrix* temp = gsl_matrix_alloc(m, n);
  XLAL_CHECK_NULL(temp != NULL, XLAL_ENOMEM);
  gsl_matrix* result = gsl_matrix_alloc(n, n);
  XLAL_CHECK_NULL(result != NULL, XLAL_ENOMEM);

  // Find the QR decomposition of the generator
  gsl_matrix_memcpy(QR_decomp, generator);
  gsl_linalg_QR_decomp(QR_decomp, QR_tau);
  gsl_linalg_QR_unpack(QR_decomp, QR_tau, Q, R);

  // Build matrix to permute column order and make signs to diagonal positive
  gsl_matrix_set_zero(perm_sign);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < m; ++j) {
      if (i + j == n - 1) {
        double x = gsl_matrix_get(R, j, j);
        gsl_matrix_set(perm_sign, i, j, x < 0 ? -1.0 : (x > 0 ? 1.0 : 0.0));
      }
    }
  }

  // Calculate left side of transform (Q is transposed to get inverse)
  gsl_blas_dgemm(CblasNoTrans, CblasTrans, 1.0, perm_sign, Q, 0.0, left);

  // Build right side of transform
  gsl_matrix_set_zero(right);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      if (i + j == n - 1) {
        gsl_matrix_set(right, i, j, 1.0);
      }
    }
  }

  // Transform generator
  gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, generator, right, 0.0, temp);
  gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, left, temp, 0.0, result);

  // Generator will be lower triangular, so zero out upper triangle
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      gsl_matrix_set(result, i, j, 0.0);
    }
  }

  // Cleanup
  gsl_matrix_free(QR_decomp);
  gsl_vector_free(QR_tau);
  gsl_matrix_free(Q);
  gsl_matrix_free(R);
  gsl_matrix_free(perm_sign);
  gsl_matrix_free(left);
  gsl_matrix_free(right);
  gsl_matrix_free(temp);

  return result;

}

///
/// Normalise a lattice generator matrix to have a specified covering radius
///
static int FLT_NormaliseLatticeGenerator(
  gsl_matrix* generator,                        ///< [in] Generator matrix of lattice
  const double norm_thickness,                  ///< [in] Normalised thickness of lattice
  const double covering_radius                  ///< [in] Desired covering radius
  )
{

  // Check input
  XLAL_CHECK(generator != NULL, XLAL_EFAULT);
  XLAL_CHECK(generator->size1 == generator->size2, XLAL_ESIZE);
  const size_t n = generator->size1;

  // Allocate memory
  gsl_matrix* LU_decomp = gsl_matrix_alloc(n, n);
  XLAL_CHECK(LU_decomp != NULL, XLAL_ENOMEM);
  gsl_permutation* LU_perm = gsl_permutation_alloc(n);
  XLAL_CHECK(LU_perm != NULL, XLAL_ENOMEM);

  // Compute generator LU decomposition
  gsl_matrix_memcpy(LU_decomp, generator);
  int LU_sign = 0;
  gsl_linalg_LU_decomp(LU_decomp, LU_perm, &LU_sign);

  // Compute generator determinant
  const double generator_determinant = gsl_linalg_LU_det(LU_decomp, LU_sign);

  // Compute covering radius
  const double generator_covering_radius = pow(norm_thickness * generator_determinant, 1.0 / n);

  // Normalise so covering spheres have specified covering radius
  gsl_matrix_scale(generator, covering_radius / generator_covering_radius);

  // Cleanup
  gsl_matrix_free(LU_decomp);
  gsl_permutation_free(LU_perm);

  return XLAL_SUCCESS;

}

///
/// Find the lattice increment vectors for a given metric and mismatch
///
static gsl_matrix* FLT_MetricLatticeIncrements(
  const FlatLatticeGenerator generator,         ///< [in] Lattice generator function
  const gsl_matrix* metric,                     ///< [in] Parameter space metric
  const double max_mismatch                     ///< [in] Maximum prescribed mismatch
  )
{

  // Check input
  XLAL_CHECK_NULL(generator != NULL, XLAL_EFAULT);
  XLAL_CHECK_NULL(metric != NULL, XLAL_EFAULT);
  XLAL_CHECK_NULL(metric->size1 == metric->size2, XLAL_ESIZE);
  XLAL_CHECK_NULL(max_mismatch > 0.0, XLAL_EINVAL);

  // Allocate memory
  gsl_matrix* directions = gsl_matrix_alloc(metric->size1, metric->size2);
  XLAL_CHECK_NULL(directions != NULL, XLAL_ENOMEM);
  gsl_matrix* increment = gsl_matrix_alloc(metric->size1, metric->size2);
  XLAL_CHECK_NULL(increment != NULL, XLAL_ENOMEM);

  // Check metric is positive definite, by trying to compute its Cholesky decomposition
  gsl_matrix_memcpy(directions, metric);   // Make copy to preserve original
  gsl_error_handler_t* old_handler = gsl_set_error_handler_off();
  int retn = gsl_linalg_cholesky_decomp(directions);
  gsl_set_error_handler(old_handler);
  XLAL_CHECK_NULL(retn == 0, XLAL_EFAILED, "metric is not positive definite");

  // Find orthonormalise directions with respect to tiling metric
  gsl_matrix_set_identity(directions);
  XLAL_CHECK_NULL(FLT_OrthonormaliseWRTMetric(directions, metric) == XLAL_SUCCESS, XLAL_EFAILED);

  // Get lattice generator
  gsl_matrix* gen_matrix = NULL;
  double norm_thickness = 0.0;
  XLAL_CHECK_NULL((generator)(metric->size1, &gen_matrix, &norm_thickness) == XLAL_SUCCESS, XLAL_EFAILED);

  // Transform lattice generator to square lower triangular
  gsl_matrix* sqlwtr_gen_matrix = FLT_SquareLowerTriangularLatticeGenerator(gen_matrix);
  XLAL_CHECK_NULL(sqlwtr_gen_matrix != NULL, XLAL_EFAILED);

  // Normalise lattice generator so covering radius is sqrt(mismatch)
  XLAL_CHECK_NULL(FLT_NormaliseLatticeGenerator(sqlwtr_gen_matrix, norm_thickness, sqrt(max_mismatch)) == XLAL_SUCCESS, XLAL_EFAILED);

  // Compute the increment vectors of the lattice generator along the orthogonal directions
  gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, 1.0, directions, sqlwtr_gen_matrix, 0.0, increment);

  // Cleanup
  gsl_matrix_free(directions);
  gsl_matrix_free(gen_matrix);
  gsl_matrix_free(sqlwtr_gen_matrix);

  return increment;

}

///
/// Get physical bounds and padding for the specified dimension
///
static void FLT_GetPhysBounds(
  FlatLatticeTiling* tiling,                    ///< [in] Tiling state
  const size_t dimension,                       ///< [in] Dimension on which bound applies
  const gsl_vector_uint* curr_bound,            ///< [in] Indices of current bounds
  const gsl_vector* phys_point,                 ///< [in] Physical point at which to find bounds
  gsl_vector* phys_lower,                       ///< [out] Physical lower bounds on point
  gsl_vector* phys_upper,                       ///< [out] Physical upper bounds on point
  double* phys_lower_pad,                       ///< [out] Physical padding of lower parameter space bounds (optional)
  double* phys_upper_pad                        ///< [out] Physical padding of upper parameter space bounds (optional)
  )
{

  // Get the appropriate bound dimension
  FLT_Bound* bound = &tiling->bounds[dimension];

  // Initialise bound vectors
  gsl_vector_set_all(phys_lower, GSL_NAN);
  gsl_vector_set_all(phys_upper, GSL_NAN);

  // Pass upper bound vector only for tiled bounds
  gsl_vector* phys_upper_tiled = bound->tiled ? phys_upper : NULL;

  // Pass physical increments only for this and lower dimensions
  gsl_vector_const_view phys_incr_view = gsl_vector_const_subvector(tiling->phys_incr, 0, dimension + 1);

  // Pass physical bounding box only for this and lower dimensions
  gsl_vector_const_view phys_bbox_view = gsl_vector_const_subvector(tiling->phys_bbox, 0, dimension + 1);

  // Use physical bounding box in this dimension as default padding;
  // for non-tiled dimensions, this will be zero for no padding
  const double phys_bbox_dim = gsl_vector_get(&phys_bbox_view.vector, dimension);
  double phys_lower_pad_val = phys_bbox_dim, phys_upper_pad_val = phys_bbox_dim;

  // Only allow padding to be modified for tiled dimensions
  double* ptr_phys_lower_pad_val = bound->tiled ? &phys_lower_pad_val : NULL;
  double* ptr_phys_upper_pad_val = bound->tiled ? &phys_upper_pad_val : NULL;

  // Call parameter space bounds function, passing view of physical point only in lower dimensions
  if (dimension == 0) {
    (bound->func)(dimension, NULL, NULL, bound->data,
                  &phys_incr_view.vector, &phys_bbox_view.vector,
                  phys_lower, phys_upper_tiled, ptr_phys_lower_pad_val, ptr_phys_upper_pad_val);
  } else {
    gsl_vector_uint_const_view curr_bound_view = gsl_vector_uint_const_subvector(curr_bound, 0, dimension);
    gsl_vector_const_view phys_point_view = gsl_vector_const_subvector(phys_point, 0, dimension);
    (bound->func)(dimension, &curr_bound_view.vector, &phys_point_view.vector, bound->data,
                  &phys_incr_view.vector, &phys_bbox_view.vector,
                  phys_lower, phys_upper_tiled, ptr_phys_lower_pad_val, ptr_phys_upper_pad_val);
  }

  // Return physical padding if required
  if (phys_lower_pad) {
    *phys_lower_pad = phys_lower_pad_val;
  }
  if (phys_upper_pad) {
    *phys_upper_pad = phys_upper_pad_val;
  }

}

FlatLatticeTiling* XLALCreateFlatLatticeTiling(
  const size_t dimensions
  )
{

  // Check input
  XLAL_CHECK_NULL(dimensions > 0, XLAL_EINVAL);
  const size_t n = dimensions;

  // Allocate and initialise tiling structure
  FlatLatticeTiling* tiling = XLALCalloc(1, sizeof(FlatLatticeTiling));
  XLAL_CHECK_NULL(tiling != NULL, XLAL_ENOMEM);
  tiling->dimensions = n;
  tiling->tiled_dimensions = 0;
  tiling->status = FLT_S_INCOMPLETE;
  tiling->generator = NULL;
  tiling->count = 0;

  // Allocate parameter space bounds info
  tiling->bounds = XLALCalloc(n, sizeof(FLT_Bound));
  XLAL_CHECK_NULL(tiling->bounds != NULL, XLAL_ENOMEM);

  // Allocate vectors and matrices
  tiling->phys_scale = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->phys_scale != NULL, XLAL_ENOMEM);
  tiling->phys_offset = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->phys_offset != NULL, XLAL_ENOMEM);
  tiling->phys_incr = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->phys_incr != NULL, XLAL_ENOMEM);
  tiling->phys_bbox = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->phys_bbox != NULL, XLAL_ENOMEM);
  tiling->metric = gsl_matrix_alloc(n, n);
  XLAL_CHECK_NULL(tiling->metric != NULL, XLAL_ENOMEM);
  tiling->increment = gsl_matrix_alloc(n, n);
  XLAL_CHECK_NULL(tiling->increment != NULL, XLAL_ENOMEM);
  tiling->curr_point = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->curr_point != NULL, XLAL_ENOMEM);
  tiling->curr_bound = gsl_vector_uint_alloc(n);
  XLAL_CHECK_NULL(tiling->curr_bound != NULL, XLAL_ENOMEM);
  tiling->curr_lower = gsl_matrix_alloc(n, MAX_BOUNDS);
  XLAL_CHECK_NULL(tiling->curr_lower != NULL, XLAL_ENOMEM);
  tiling->curr_upper = gsl_matrix_alloc(n, MAX_BOUNDS);
  XLAL_CHECK_NULL(tiling->curr_upper != NULL, XLAL_ENOMEM);
  tiling->curr_lower_pad = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->curr_lower_pad != NULL, XLAL_ENOMEM);
  tiling->curr_upper_pad = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->curr_upper_pad != NULL, XLAL_ENOMEM);
  tiling->curr_phys_point = gsl_vector_alloc(n);
  XLAL_CHECK_NULL(tiling->curr_phys_point != NULL, XLAL_ENOMEM);

  return tiling;

}

void XLALDestroyFlatLatticeTiling(
  FlatLatticeTiling* tiling
  )
{

  if (tiling) {

    const size_t n = tiling->dimensions;

    gsl_error_handler_t* old_handler = gsl_set_error_handler_off();

    // Free bounds data, allowing bounds to share the same memory
    for (size_t i = 0; i < n; ++i) {
      void* data = tiling->bounds[i].data;
      if (data != NULL) {
        for (size_t j = i; j < n; ++j) {
          if (tiling->bounds[j].data == data) {
            tiling->bounds[j].data = NULL;
          }
        }
        XLALFree(data);
      }
    }
    XLALFree(tiling->bounds);

    // Free vectors and matrices
    gsl_vector_free(tiling->phys_scale);
    gsl_vector_free(tiling->phys_offset);
    gsl_vector_free(tiling->phys_incr);
    gsl_vector_free(tiling->phys_bbox);
    gsl_matrix_free(tiling->metric);
    gsl_matrix_free(tiling->increment);
    gsl_vector_free(tiling->curr_point);
    gsl_vector_uint_free(tiling->curr_bound);
    gsl_matrix_free(tiling->curr_lower);
    gsl_matrix_free(tiling->curr_upper);
    gsl_vector_free(tiling->curr_lower_pad);
    gsl_vector_free(tiling->curr_upper_pad);
    gsl_vector_free(tiling->curr_phys_point);

    // Free tiling structure
    XLALFree(tiling);

    gsl_set_error_handler(old_handler);

  }

}

size_t XLALGetFlatLatticeDimensions(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK_VAL(0, tiling != NULL, XLAL_EFAULT);

  return tiling->dimensions;

}

const gsl_vector* XLALGetFlatLatticePoint(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK_NULL(tiling != NULL, XLAL_EFAULT);

  return tiling->status == FLT_S_STARTED ? tiling->curr_phys_point : NULL;

}

unsigned long XLALGetFlatLatticePointCount(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK_VAL(0, tiling != NULL, XLAL_EFAULT);

  return tiling->count;

}

gsl_matrix* XLALGetFlatLatticeIncrements(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK_NULL(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK_NULL(tiling->status == FLT_S_INITIALISED, XLAL_EFAILED);

  // Allocate increment vector
  gsl_matrix* increment = gsl_matrix_alloc(tiling->increment->size1, tiling->increment->size2);
  XLAL_CHECK_NULL(tiling->increment != NULL, XLAL_ENOMEM);

  // Copy increments, rescaled to physical coordinates
  for (size_t i = 0; i < increment->size2; ++i) {
    gsl_vector_view tiling_increment_i = gsl_matrix_column(tiling->increment, i);
    gsl_vector_view increment_i = gsl_matrix_column(increment, i);
    gsl_vector_memcpy(&increment_i.vector, &tiling_increment_i.vector);
    gsl_vector_mul(&increment_i.vector, tiling->phys_scale);
  }

  return increment;

}

int XLALSetFlatLatticeBound(
  FlatLatticeTiling* tiling,
  const size_t dimension,
  const bool singular,
  const FlatLatticeBound func,
  void* data
  )
{

  // Check tiling
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(tiling->status == FLT_S_INCOMPLETE, XLAL_EFAILED);

  // Check input
  XLAL_CHECK(dimension < tiling->dimensions, XLAL_ESIZE);
  XLAL_CHECK(func != NULL, XLAL_EFAULT);

  // Set the next parameter space bound
  tiling->bounds[dimension].tiled = !singular;
  tiling->bounds[dimension].func = func;
  tiling->bounds[dimension].data = data;

  return XLAL_SUCCESS;

}

int XLALSetFlatLatticeGenerator(
  FlatLatticeTiling* tiling,
  const FlatLatticeGenerator generator
  )
{

  // Check tiling
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(tiling->status == FLT_S_INCOMPLETE, XLAL_EFAILED);

  // Check input
  XLAL_CHECK(generator != NULL, XLAL_EFAILED);

  // Set the flat lattice tiling generator
  tiling->generator = generator;

  return XLAL_SUCCESS;

}

int XLALSetFlatLatticeMetric(
  FlatLatticeTiling* tiling,
  const gsl_matrix* metric,
  const double max_mismatch
  )
{

  const size_t n = tiling->dimensions;

  // Check tiling
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(tiling->status == FLT_S_INCOMPLETE, XLAL_EFAILED);

  // Check input
  XLAL_CHECK(metric != NULL, XLAL_EFAULT);
  XLAL_CHECK(metric->size1 == n && metric->size2 == n, XLAL_EINVAL);
  XLAL_CHECK(max_mismatch > 0, XLAL_EINVAL);

  // Check that all parameter space dimensions are bounded,
  // and count number of tiles dimensions
  tiling->tiled_dimensions = 0;
  for (size_t i = 0; i < tiling->dimensions; ++i) {
    XLAL_CHECK(tiling->bounds[i].func != NULL, XLAL_EFAILED, "Dimension #%i is unbounded", i);
    tiling->tiled_dimensions += tiling->bounds[i].tiled ? 1 : 0;
  }

  // Check that the flat lattice tiling generator has been set
  XLAL_CHECK(tiling->generator != NULL, XLAL_EFAILED);

  // Initialise parameter space bound indices
  gsl_vector_uint_set_zero(tiling->curr_bound);

  // Get physical parameter space offset
  for (size_t i = 0; i < n; ++i) {

    // Get physical bounds and padding
    gsl_vector_view phys_lower = gsl_matrix_row(tiling->curr_lower, i);
    gsl_vector_view phys_upper = gsl_matrix_row(tiling->curr_upper, i);
    FLT_GetPhysBounds(tiling, i, tiling->curr_bound, tiling->phys_offset,
                      &phys_lower.vector, &phys_upper.vector, NULL, NULL);

    // Set physical parameter space offset
    gsl_vector_set(tiling->phys_offset, i, gsl_vector_get(&phys_lower.vector, 0));

  }

  // Check diagonal elements of tiled dimensions are positive, and calculate
  // physical parameter space scaling from metric diagonal elements
  gsl_vector_set_all(tiling->phys_scale, 1.0);
  for (size_t i = 0; i < n; ++i) {
    if (tiling->bounds[i].tiled) {
      const double metric_i_i = gsl_matrix_get(metric, i, i);
      XLAL_CHECK(metric_i_i > 0, XLAL_EINVAL, "metric(%zu,%zu) <= 0", i, i);
      gsl_vector_set(tiling->phys_scale, i, 1.0 / sqrt(metric_i_i));
    }
  }

  // Check metric is symmetric, and copy rescaled metric
  for (size_t i = 0; i < n; ++i) {
    const double scale_i = gsl_vector_get(tiling->phys_scale, i);
    for (size_t j = 0; j < n; ++j) {
      const double scale_j = gsl_vector_get(tiling->phys_scale, j);
      double metric_i_j = gsl_matrix_get(metric, i, j);
      XLAL_CHECK(metric_i_j == gsl_matrix_get(metric, j, i), XLAL_EINVAL, "metric(%zu,%zu) != metric(%zu,%zu)", i, j, j, i);
      metric_i_j *= scale_i * scale_j;
      gsl_matrix_set(tiling->metric, i, j, metric_i_j);
    }
  }

  // Initialise for zero-dimensional parameter space
  gsl_vector_set_zero(tiling->phys_incr);
  gsl_vector_set_zero(tiling->phys_bbox);
  gsl_matrix_set_zero(tiling->increment);

  if (tiling->tiled_dimensions > 0) {

    const size_t tn = tiling->tiled_dimensions;

    // Allocate memory
    gsl_matrix* tmetric = gsl_matrix_alloc(tn, tn);
    XLAL_CHECK(tmetric != NULL, XLAL_ENOMEM);

    // Copy tiled dimensions of metric
    for (size_t i = 0, ti = 0; i < n; ++i) {
      if (tiling->bounds[i].tiled) {
        for (size_t j = 0, tj = 0; j < n; ++j) {
          if (tiling->bounds[j].tiled) {
            gsl_matrix_set(tmetric, ti, tj, gsl_matrix_get(tiling->metric, i, j));
            ++tj;
          }
        }
        ++ti;
      }
    }

    // Calculate metric lattice increment vectors
    gsl_matrix* tincrement = FLT_MetricLatticeIncrements(tiling->generator, tmetric, max_mismatch);
    XLAL_CHECK(tincrement != NULL, XLAL_EFAILED);

    // Calculate metric ellipse bounding box
    gsl_vector* tbounding_box = FLT_MetricEllipseBoundingBox(tmetric, max_mismatch);
    XLAL_CHECK(tbounding_box != NULL, XLAL_EFAILED);

    // Copy increment vectors and bounding box so that non-tiled dimensions are zero
    for (size_t i = 0, ti = 0; i < n; ++i) {
      if (tiling->bounds[i].tiled) {
        gsl_vector_set(tiling->phys_incr, i, gsl_matrix_get(tincrement, ti, ti));
        gsl_vector_set(tiling->phys_bbox, i, gsl_vector_get(tbounding_box, ti));
        for (size_t j = 0, tj = 0; j < n; ++j) {
          if (tiling->bounds[j].tiled) {
            gsl_matrix_set(tiling->increment, i, j, gsl_matrix_get(tincrement, ti, tj));
            ++tj;
          }
        }
        ++ti;
      }
    }

    // Convert increments and bounding box to physical coordinates
    gsl_vector_mul(tiling->phys_incr, tiling->phys_scale);
    gsl_vector_mul(tiling->phys_bbox, tiling->phys_scale);

    // Cleanup
    gsl_matrix_free(tmetric);
    gsl_matrix_free(tincrement);
    gsl_vector_free(tbounding_box);

  }

  // Tiling has been fully initialised
  tiling->status = FLT_S_INITIALISED;
  XLALRestartFlatLatticeTiling(tiling);

  return XLAL_SUCCESS;

}

int XLALNextFlatLatticePoint(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(tiling->status != FLT_S_INCOMPLETE, XLAL_EFAILED);
  const size_t n = tiling->dimensions;

  // If finished status, nothing more to be done!
  if (tiling->status == FLT_S_FINISHED) {
    return -1;
  }

  // If started status, but no tiled dimensions, we're finished!
  if (tiling->status == FLT_S_STARTED && tiling->tiled_dimensions == 0) {
    tiling->status = FLT_S_FINISHED;
    return -1;
  }

  // If initialised status, set and return starting point
  if (tiling->status == FLT_S_INITIALISED) {

    // Initialise parameter space bound indices
    gsl_vector_uint_set_zero(tiling->curr_bound);

    // Set parameter space bounds and starting point
    for (size_t i = 0; i < n; ++i) {

      // Get physical scales and offsets
      const double phys_scale = gsl_vector_get(tiling->phys_scale, i);
      const double phys_offset = gsl_vector_get(tiling->phys_offset, i);

      // Get physical bounds and padding
      gsl_vector_view phys_lower = gsl_matrix_row(tiling->curr_lower, i);
      gsl_vector_view phys_upper = gsl_matrix_row(tiling->curr_upper, i);
      double phys_lower_pad = 0, phys_upper_pad = 0;
      FLT_GetPhysBounds(tiling, i, tiling->curr_bound, tiling->curr_phys_point,
                        &phys_lower.vector, &phys_upper.vector, &phys_lower_pad, &phys_upper_pad);

      // Normalise physical bounds and padding
      gsl_vector_add_constant(&phys_lower.vector, -phys_offset);
      gsl_vector_scale(&phys_lower.vector, 1.0/phys_scale);
      gsl_vector_add_constant(&phys_upper.vector, -phys_offset);
      gsl_vector_scale(&phys_upper.vector, 1.0/phys_scale);
      gsl_vector_set(tiling->curr_lower_pad, i, phys_lower_pad / phys_scale);
      gsl_vector_set(tiling->curr_upper_pad, i, phys_upper_pad / phys_scale);

      // Initialise current point
      const size_t bound = gsl_vector_uint_get(tiling->curr_bound, i);
      double point = gsl_matrix_get(tiling->curr_lower, i, bound);
      point -= gsl_vector_get(tiling->curr_lower_pad, i);
      gsl_vector_set(tiling->curr_point, i, point);

      // Update current physical point
      const double phys_point = (point * phys_scale) + phys_offset;
      gsl_vector_set(tiling->curr_phys_point, i, phys_point);

    }

    // Update current physical point
    gsl_vector_memcpy(tiling->curr_phys_point, tiling->curr_point);
    gsl_vector_mul(tiling->curr_phys_point, tiling->phys_scale);
    gsl_vector_add(tiling->curr_phys_point, tiling->phys_offset);

    // Initialise count
    tiling->count = 1;

    // Tiling has been started.
    tiling->status = FLT_S_STARTED;

    // All dimensions of point have changed.
    return 0;

  }

  // Otherwise started status: loop until the next point is found
  size_t i = n, ir;
  while (true) {

    // If dimension index is now zero, we're done!
    if (i == 0) {
      tiling->status = FLT_S_FINISHED;
      return -1;
    }

    // Decrement current dimension index
    --i;

    // Return point to lower bound in higher dimensions
    ir = i + 1;

    // Get current bound index
    size_t bound = gsl_vector_uint_get(tiling->curr_bound, i);

    // If dimension is tiled...
    if (tiling->bounds[i].tiled) {

      // Get increment vector
      gsl_vector_view increment = gsl_matrix_column(tiling->increment, i);

      // Increment current point along index
      gsl_vector_add(tiling->curr_point, &increment.vector);

      // Update current physical point
      gsl_vector_memcpy(tiling->curr_phys_point, tiling->curr_point);
      gsl_vector_mul(tiling->curr_phys_point, tiling->phys_scale);
      gsl_vector_add(tiling->curr_phys_point, tiling->phys_offset);

      // If point is not out of bounds, we have found a template point
      const double point = gsl_vector_get(tiling->curr_point, i);
      const double upper = gsl_matrix_get(tiling->curr_upper, i, bound);
      const double upper_pad = gsl_vector_get(tiling->curr_upper_pad, i);
      if (point <= upper + upper_pad) {
        break;
      }

    }

    // Increment bound index
    ++bound;

    if (bound < MAX_BOUNDS) {

      // Get bounds
      const double lower = gsl_matrix_get(tiling->curr_lower, i, bound);
      const double upper = gsl_matrix_get(tiling->curr_upper, i, bound);

      if (gsl_isnan(lower) && gsl_isnan(upper)) {

        // If no more bounds, reset bound index in this and higher dimensions
        for (size_t j = i; j < n; ++j) {
          gsl_vector_uint_set(tiling->curr_bound, j, 0);
        }

      } else {

        // Set current bound index
        gsl_vector_uint_set(tiling->curr_bound, i, bound);

        // Return point to new lower bound in this dimension
        ir = i;

        // Found a template point
        break;

      }

    }

    // Move on to lower dimensions
    continue;

  }

  // Return point to lower bound in appropriate dimensions
  for (; ir < n; ++ir) {

    // Get current bound index
    const size_t bound = gsl_vector_uint_get(tiling->curr_bound, ir);

    // Get new physical bounds if required
    if (bound == 0) {

      // Get physical scales and offsets
      const double phys_scale = gsl_vector_get(tiling->phys_scale, ir);
      const double phys_offset = gsl_vector_get(tiling->phys_offset, ir);

      // Get physical bounds and padding
      gsl_vector_view phys_lower = gsl_matrix_row(tiling->curr_lower, ir);
      gsl_vector_view phys_upper = gsl_matrix_row(tiling->curr_upper, ir);
      double phys_lower_pad = 0, phys_upper_pad = 0;
      FLT_GetPhysBounds(tiling, ir, tiling->curr_bound, tiling->curr_phys_point,
                        &phys_lower.vector, &phys_upper.vector, &phys_lower_pad, &phys_upper_pad);

      // Normalise physical bounds and padding
      gsl_vector_add_constant(&phys_lower.vector, -phys_offset);
      gsl_vector_scale(&phys_lower.vector, 1.0/phys_scale);
      gsl_vector_add_constant(&phys_upper.vector, -phys_offset);
      gsl_vector_scale(&phys_upper.vector, 1.0/phys_scale);
      gsl_vector_set(tiling->curr_lower_pad, ir, phys_lower_pad / phys_scale);
      gsl_vector_set(tiling->curr_upper_pad, ir, phys_upper_pad / phys_scale);

    }

    // Get lower bound
    const double lower = gsl_matrix_get(tiling->curr_lower, ir, bound);

    // If dimension is tiled...
    if (tiling->bounds[ir].tiled) {

      // Get increment vector
      gsl_vector_view increment = gsl_matrix_column(tiling->increment, ir);

      // Calculate the distance from current point to the lower bound, in integer number of increments
      const double lower_pad = gsl_vector_get(tiling->curr_lower_pad, ir);
      const double point = gsl_vector_get(tiling->curr_point, ir);
      const double dist = ceil((lower - lower_pad - point) / gsl_vector_get(&increment.vector, ir));

      // Move point back to lower bound
      gsl_blas_daxpy(dist, &increment.vector, tiling->curr_point);

    } else {

      // Otherwise set point to lower bound
      gsl_vector_set(tiling->curr_point, ir, lower);

    }

    // Update current physical point
    gsl_vector_memcpy(tiling->curr_phys_point, tiling->curr_point);
    gsl_vector_mul(tiling->curr_phys_point, tiling->phys_scale);
    gsl_vector_add(tiling->curr_phys_point, tiling->phys_offset);

  }

  // Template was found, so increase count
  ++tiling->count;

  // Return lowest dimension where point has changed
  return i;

}

int XLALRestartFlatLatticeTiling(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(tiling->status != FLT_S_INCOMPLETE, XLAL_EFAILED);

  // Restart tiling
  tiling->status = FLT_S_INITIALISED;
  tiling->count = 0;

  return XLAL_SUCCESS;

}

unsigned long XLALCountTotalFlatLatticePoints(
  FlatLatticeTiling* tiling
  )
{

  // Check tiling
  XLAL_CHECK_VAL(0, tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK_VAL(0, tiling->status != FLT_S_INCOMPLETE, XLAL_EFAILED);

  // Iterate over all templates
  while (XLALNextFlatLatticePoint(tiling) >= 0);
  XLAL_CHECK_VAL(0, xlalErrno == 0, XLAL_EFAILED);

  // Save the template count
  unsigned long count = tiling->count;

  // Restart tiling
  XLALRestartFlatLatticeTiling(tiling);

  // Return the template count
  return count;

}

int XLALNearestFlatLatticePointToRandomPoints(
  FlatLatticeTiling* tiling,
  RandomParams* rng,
  const size_t num_random_points,
  gsl_matrix** random_points,
  gsl_vector_ulong** nearest_indices,
  gsl_vector** nearest_distances,
  gsl_matrix** workspace
  )
{

  // Check tiling
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(tiling->status != FLT_S_INCOMPLETE, XLAL_EFAILED);
  const size_t n = tiling->dimensions;

  // Check input
  XLAL_CHECK(rng != NULL, XLAL_EFAULT);
  XLAL_CHECK(num_random_points > 0, XLAL_ESIZE);
  XLAL_CHECK(random_points != NULL, XLAL_EFAULT);
  XLAL_CHECK(nearest_indices != NULL, XLAL_EFAULT);
  XLAL_CHECK(nearest_distances != NULL, XLAL_EFAULT);
  XLAL_CHECK(workspace != NULL, XLAL_EFAULT);

  // (Re)Allocate matrix of random points
  if (*random_points != NULL && (*random_points)->size2 != num_random_points) {
    gsl_matrix_free(*random_points);
    *random_points = NULL;
  }
  if (*random_points == NULL) {
    *random_points = gsl_matrix_alloc(n, num_random_points);
    XLAL_CHECK(*random_points != NULL, XLAL_ENOMEM);
  }

  // (Re)Allocate vector of indices of nearest lattice point
  if (*nearest_indices != NULL && (*nearest_indices)->size != num_random_points) {
    gsl_vector_ulong_free(*nearest_indices);
    *nearest_indices = NULL;
  }
  if (*nearest_indices == NULL) {
    *nearest_indices = gsl_vector_ulong_alloc(num_random_points);
    XLAL_CHECK(*nearest_indices != NULL, XLAL_ENOMEM);
  }

  // (Re)Allocate vector of distances to nearest lattice point
  if (*nearest_distances != NULL && (*nearest_distances)->size != num_random_points) {
    gsl_vector_free(*nearest_distances);
    *nearest_distances = NULL;
  }
  if (*nearest_distances == NULL) {
    *nearest_distances = gsl_vector_alloc(num_random_points);
    XLAL_CHECK(*nearest_distances != NULL, XLAL_ENOMEM);
  }

  // (Re)Allocate workspace matrix for computing distances
  if (*workspace != NULL && (*workspace)->size2 != num_random_points) {
    gsl_matrix_free(*workspace);
    *workspace = NULL;
  }
  if (*workspace == NULL) {
    *workspace = gsl_matrix_alloc(3*n - 1, num_random_points);
    XLAL_CHECK(*workspace != NULL, XLAL_ENOMEM);
  }

  // Create temporary bound index and physical bound vectors
  gsl_vector_uint* curr_bound = gsl_vector_uint_alloc(n);
  XLAL_CHECK(curr_bound != NULL, XLAL_ENOMEM);
  gsl_vector* phys_lower = gsl_vector_alloc(MAX_BOUNDS);
  XLAL_CHECK(phys_lower != NULL, XLAL_ENOMEM);
  gsl_vector* phys_width = gsl_vector_alloc(MAX_BOUNDS);
  XLAL_CHECK(phys_width != NULL, XLAL_ENOMEM);

  // Create random points in flat lattice tiling parameter space
  for (size_t k = 0; k < num_random_points; ++k) {
    gsl_vector_view point = gsl_matrix_column(*random_points, k);
    for (size_t i = 0; i < n; ++i) {

      // Get physical bounds and padding
      FLT_GetPhysBounds(tiling, i, curr_bound, &point.vector,
                        phys_lower, phys_width, NULL, NULL);
      gsl_vector_sub(phys_width, phys_lower);

      // Get total bounds width
      double phys_total_width = 0;
      size_t max_bounds = 0;
      while (max_bounds < MAX_BOUNDS) {
        const double lower = gsl_vector_get(phys_lower, max_bounds);
        const double width = gsl_vector_get(phys_width, max_bounds);
        if (gsl_isnan(lower) && gsl_isnan(width)) {
          break;
        }
        phys_total_width += width;
        ++max_bounds;
      }

      // Generate random number
      const double u = XLALUniformDeviate(rng);

      double p;
      size_t bound = 0;
      if (tiling->bounds[i].tiled) {

        // Generate random point within total bounds widths
        p = u * phys_total_width;

        // Convert point to be within parameter space bounds
        while (bound + 1 < max_bounds) {
          const double width = gsl_vector_get(phys_width, bound);
          if (p <= width) {
            break;
          }
          p -= width;
          ++bound;
        }
        p += gsl_vector_get(phys_lower, bound);

      } else {

        // Generate random bound index
        bound = (size_t)floor(u * max_bounds);

        // Get point from random bound
        p = gsl_vector_get(phys_lower, bound);

      }

      // Set parameter space point and bound index
      gsl_vector_set(&point.vector, i, p);
      gsl_vector_uint_set(curr_bound, i, bound);

    }

  }

  // Create temporary matrices in workspace
  gsl_matrix_view point_diffs = gsl_matrix_submatrix(*workspace, 0, 0, n, num_random_points);
  gsl_matrix_view off_diag_terms = gsl_matrix_submatrix(*workspace, n, 0, n - 1, num_random_points);
  gsl_matrix_view distances = gsl_matrix_submatrix(*workspace, 2*n - 1, 0, n, num_random_points);

  // Initialise minimum distance vector
  gsl_vector_set_all(*nearest_distances, GSL_POSINF);

  // Iterate over all flat lattice points
  XLALRestartFlatLatticeTiling(tiling);
  while (true) {

    // Advance to the next lattice point
    const int ich = XLALNextFlatLatticePoint(tiling);
    if (ich < 0) {
      break;
    }
    const gsl_vector* lattice_point = XLALGetFlatLatticePoint(tiling);
    const unsigned long nearest_index = tiling->count - 1;

    // For dimensions where flat lattice point has changed (given by ich),
    // copy random points to workspace, subtract flat lattice point from each,
    // and normalise by physical scaling
    for (size_t i = (size_t)ich; i < n; ++i) {
      const double phys_scale = gsl_vector_get(tiling->phys_scale, i);
      gsl_vector_view point_diffs_i = gsl_matrix_row(&point_diffs.matrix, i);
      gsl_vector_view random_points_i = gsl_matrix_row(*random_points, i);
      gsl_vector_memcpy(&point_diffs_i.vector, &random_points_i.vector);
      gsl_vector_add_constant(&point_diffs_i.vector, -gsl_vector_get(lattice_point, i));
      gsl_vector_scale(&point_diffs_i.vector, 1.0/phys_scale);
    }

    // For dimensions where flat lattice point has changed (given by ich),
    // re-compute the off-diagonal terms of the metric distance, which
    // are multiplied by the ith coordinate difference
    for (size_t i = (size_t)ich; i < n - 1; ++i) {
      gsl_vector_view off_diag_terms_i = gsl_matrix_row(&off_diag_terms.matrix, i);
      gsl_vector_set_zero(&off_diag_terms_i.vector);
      for (size_t j = 0; j <= i; ++j) {
        const double metric_off_diag = gsl_matrix_get(tiling->metric, i + 1, j);
        gsl_vector_view point_diffs_j = gsl_matrix_row(&point_diffs.matrix, j);
        gsl_blas_daxpy(2.0 * metric_off_diag, &point_diffs_j.vector, &off_diag_terms_i.vector);
      }
    }

    // For dimensions where flat lattice point has changed (given by ich),
    // re-compute terms in the distances from random points to the flat lattice
    // point which involve the ith coordinate difference, and cumulatively sum
    // together to get the distance in the last row
    for (size_t i = (size_t)ich; i < n; ++i) {

      gsl_vector_view point_diffs_i = gsl_matrix_row(&point_diffs.matrix, i);
      gsl_vector_view distances_i = gsl_matrix_row(&distances.matrix, i);

      // Compute the diagonal term of the metric distance,
      // which are multiplied by the ith coordinate difference
      const double metric_diag = gsl_matrix_get(tiling->metric, i, i);
      gsl_vector_memcpy(&distances_i.vector, &point_diffs_i.vector);
      gsl_vector_scale(&distances_i.vector, metric_diag);

      // Add the pre-computed off-diagomal terms of the metric distance,
      // which are multiplied by the ith coordinate difference
      if (i > 0) {
        gsl_vector_view off_diag_terms_iprev = gsl_matrix_row(&off_diag_terms.matrix, i - 1);
        gsl_vector_add(&distances_i.vector, &off_diag_terms_iprev.vector);
      }

      // Multiply by the ith coordinate difference
      gsl_vector_mul(&distances_i.vector, &point_diffs_i.vector);

      // Add the distance computed for the lower dimensions thus far
      if (i > 0) {
        gsl_vector_view distances_iprev = gsl_matrix_row(&distances.matrix, i - 1);
        gsl_vector_add(&distances_i.vector, &distances_iprev.vector);
      }

    }

    // For each random point, if the distance to the flat lattice point is
    // the smallest so far, record the flat lattice point, distance, and index
    gsl_vector_view distance = gsl_matrix_row(&distances.matrix, n - 1);
    for (size_t k = 0; k < num_random_points; ++k) {
      const double distance_k = gsl_vector_get(&distance.vector, k);
      if (distance_k < gsl_vector_get(*nearest_distances, k)) {
        gsl_vector_ulong_set(*nearest_indices, k, nearest_index);
        gsl_vector_set(*nearest_distances, k, distance_k);
      }
    }

  }
  XLAL_CHECK(xlalErrno == 0, XLAL_EFAILED, "XLALNextFlatLatticePoint() failed");

  // Cleanup
  gsl_vector_uint_free(curr_bound);
  gsl_vector_free(phys_lower);
  gsl_vector_free(phys_width);

  return XLAL_SUCCESS;

}

int XLALCubicLatticeGenerator(
  const size_t dimensions,
  gsl_matrix** generator,
  double* norm_thickness
  )
{

  const size_t r = dimensions;

  // Check input
  XLAL_CHECK(generator != NULL, XLAL_EFAULT);
  XLAL_CHECK(norm_thickness != NULL, XLAL_EFAULT);

  // Allocate memory
  *generator = gsl_matrix_alloc(r, r);
  XLAL_CHECK(*generator != NULL, XLAL_ENOMEM);

  // Create generator
  gsl_matrix_set_identity(*generator);

  // Calculate normalised thickness
  *norm_thickness = pow(sqrt(r)/2, r);

  return XLAL_SUCCESS;

}

int XLALAnstarLatticeGenerator(
  const size_t dimensions,
  gsl_matrix** generator,
  double* norm_thickness
  )
{

  const size_t r = dimensions;

  // Check input
  XLAL_CHECK(generator != NULL, XLAL_EFAULT);
  XLAL_CHECK(norm_thickness != NULL, XLAL_EFAULT);

  // Allocate memory
  *generator = gsl_matrix_alloc(r + 1, r);
  XLAL_CHECK(*generator != NULL, XLAL_ENOMEM);

  // Create generator in (r + 1) space
  gsl_matrix_set_all(*generator, 0.0);
  {
    gsl_vector_view first_row = gsl_matrix_row(*generator, 0);
    gsl_vector_view sub_diag = gsl_matrix_subdiagonal(*generator, 1);
    gsl_vector_view last_col = gsl_matrix_column(*generator, r - 1);
    gsl_vector_set_all(&first_row.vector, 1.0);
    gsl_vector_set_all(&sub_diag.vector, -1.0);
    gsl_vector_set_all(&last_col.vector, 1.0 / (r + 1.0));
    gsl_vector_set(&last_col.vector, 0, -1.0 * r / (r + 1.0));
  }

  // Calculate normalised thickness
  *norm_thickness = sqrt(r + 1.0)*pow((1.0*r*(r + 2))/(12.0*(r + 1)), 0.5*r);

  return XLAL_SUCCESS;

}

static void ConstantBound(
  const size_t dimension UNUSED,
  const gsl_vector_uint* bound UNUSED,
  const gsl_vector* point UNUSED,
  const void* data,
  const gsl_vector* incr UNUSED,
  const gsl_vector* bbox UNUSED,
  gsl_vector* lower,
  gsl_vector* upper,
  double* lower_pad UNUSED,
  double* upper_pad UNUSED
  )
{

  // Get bounds data
  const double* bounds = (const double*)data;

  // Set constant lower and upper bounds
  gsl_vector_set(lower, 0, bounds[0]);
  if (upper) {
    gsl_vector_set(upper, 0, bounds[1]);
  }

}

int XLALSetFlatLatticeConstantBound(
  FlatLatticeTiling* tiling,
  size_t dimension,
  double bound1,
  double bound2
  )
{

  // Check input
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(isfinite(bound1), XLAL_EINVAL);
  XLAL_CHECK(isfinite(bound2), XLAL_EINVAL);

  // Allocate and set bounds data
  double* bounds = XLALCalloc(2, sizeof(double));
  XLAL_CHECK(bounds != NULL, XLAL_ENOMEM);
  bounds[0] = GSL_MIN(bound1, bound2);
  bounds[1] = GSL_MAX(bound1, bound2);

  // Set parameter space bound
  XLAL_CHECK(XLALSetFlatLatticeBound(tiling, dimension, bound1 == bound2, ConstantBound, (void*)bounds) == XLAL_SUCCESS, XLAL_EFAILED);

  return XLAL_SUCCESS;

}

static void EllipticalYBound(
  const size_t dimension,
  const gsl_vector_uint* bound UNUSED,
  const gsl_vector* point,
  const void* data,
  const gsl_vector* incr UNUSED,
  const gsl_vector* bbox,
  gsl_vector* lower,
  gsl_vector* upper,
  double* lower_pad,
  double* upper_pad
  )
{

  // Get bounds data
  const double* bounds = (const double*)data;
  const double x_centre = bounds[0];
  const double y_centre = bounds[1];
  const double x_semi = bounds[2];
  const double y_semi = bounds[3];

  // Get normalised, centred x coordinate
  const double nx = (gsl_vector_get(point, dimension - 1) - x_centre) / x_semi;

  // Set bounds on y coordinate
  const double nxsqr = nx * nx;
  const double ny = (nxsqr < 1.0) ? sqrt(1.0 - nxsqr) : 0.0;
  gsl_vector_set(lower, 0, y_centre - ny * y_semi);
  gsl_vector_set(upper, 0, y_centre + ny * y_semi);

  // Add sufficient extra padding on y, such that the bounding box of the
  // boundary templates will not intersect the elliptic x-y parameter space.
  const double nhbbx = 0.5 * gsl_vector_get(bbox, dimension - 1) / x_semi;
  const double absnx = fabs(nx);
  double npy = 0.0;
  if (absnx <= nhbbx) {
    npy = 1.0 - ny;
  } else if (absnx <= 1.0 + nhbbx) {
    const double dnx = (nx < 0.0) ? nx + nhbbx : nx - nhbbx;
    npy = sqrt(1.0 - dnx * dnx) - ny;
  }
  const double pad = npy * y_semi;
  *lower_pad += pad;
  *upper_pad += pad;

}

int XLALSetFlatLatticeEllipticalBounds(
  FlatLatticeTiling* tiling,
  const size_t x_dimension,
  const double x_centre,
  const double y_centre,
  const double x_semi,
  const double y_semi
  )
{

  // Check input
  XLAL_CHECK(tiling != NULL, XLAL_EFAULT);
  XLAL_CHECK(x_semi >= 0.0, XLAL_EINVAL);
  XLAL_CHECK(y_semi >= 0.0, XLAL_EINVAL);

  // Set parameter space X bound
  XLAL_CHECK(XLALSetFlatLatticeConstantBound(tiling, x_dimension, x_centre - x_semi, x_centre + x_semi) == XLAL_SUCCESS, XLAL_EFAILED);

  // Set parameter space Y bound
  if (x_semi == 0.0 || y_semi == 0.0) {
    XLAL_CHECK(XLALSetFlatLatticeConstantBound(tiling, x_dimension + 1, y_centre - y_semi, y_centre + y_semi) == XLAL_SUCCESS, XLAL_EFAILED);
  } else {

    // Allocate and set bounds data
    double* bounds = XLALCalloc(4, sizeof(double));
    XLAL_CHECK(bounds != NULL, XLAL_ENOMEM);
    bounds[0] = x_centre;
    bounds[1] = y_centre;
    bounds[2] = x_semi;
    bounds[3] = y_semi;

    // Set parameter space bound
    XLAL_CHECK(XLALSetFlatLatticeBound(tiling, x_dimension + 1, false, EllipticalYBound, (void*)bounds) == XLAL_SUCCESS, XLAL_EFAILED);

  }

  return XLAL_SUCCESS;

}
