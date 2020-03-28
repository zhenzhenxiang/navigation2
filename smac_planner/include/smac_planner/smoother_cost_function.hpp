// Copyright (c) 2020, Samsung Research America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License. Reserved.

#ifndef SMAC_PLANNER__SMOOTHER_COST_FUNCTION_HPP_
#define SMAC_PLANNER__SMOOTHER_COST_FUNCTION_HPP_

#include <cmath>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <memory>
#include <queue>
#include <utility>

#include "ceres/ceres.h"
#include "Eigen/Core"
#include "smac_planner/types.hpp"
#include "smac_planner/minimal_costmap.hpp"

#define EPSILON 0.001  

// [done] waypoint smoothing term (smoothing)
// [done] max curvature term (curvature)
// smooth curvature term (new, me)
// collision obsstacle term (collision)
// smooth obstacle term (vornoi)

// TODO how to test each term and make sure wont/doesnt break?

/*
Other errors:
[planner_server-8] W0320 18:48:15.573891 16679 line_search.cc:758] Line search failed: Wolfe zoom bracket width: 7.87789e-10 too small with descent_direction_max_norm: 2.31001e-02.
[planner_server-8] Termination:                          FAILURE (Numerical failure in line search, failed to find a valid step size, (did not run out of iterations) using initial_step_size: 1.00000e+00, initial_cost: 2.85747e-04, initial_gradient: -1.44590e-06.)
[planner_server-8] W0320 22:10:09.060544 26622 line_search.cc:584] Line search failed: Wolfe bracketing phase shrank bracket width: 3.22458e-06, to < tolerance: 1e-09, with descent_direction_max_norm: 0.000251634, and failed to find a point satisfying the strong Wolfe conditions or a bracketing containing such a point. Accepting point found satisfying Armijo condition only, to allow continuation.
[planner_server-8] W0320 22:10:09.061851 26622 line_search.cc:726] Line search failed: Wolfe zoom phase passed a bracket which does not satisfy: bracket_low.gradient * (bracket_high.x - bracket_low.x) < 0 [1.91737173e-12 !< 0] with initial_position: [x: 0.00000000e+00, value: 2.85747015e-04, gradient: -1.44589730e-06, value_is_valid: 1, gradient_is_valid: 1], bracket_low: [x: 1.07133785e+00, value: 2.80767640e-04, gradient: -1.37853074e-06, value_is_valid: 1, gradient_is_valid: 1], bracket_high: [x: 1.07133646e+00, value: 2.80767646e-04, gradient: -1.37853083e-06, value_is_valid: 1, gradient_is_valid: 1], the most likely cause of which is the cost function returning inconsistent gradient & function values.
[planner_server-8] W0320 22:10:09.063752 26622 line_search_minimizer.cc:318] Terminating: Numerical failure in line search, failed to find a valid step size, (did not run out of iterations) using initial_step_size: 1.00000e+00, initial_cost: 2.85747e-04, initial_gradient: -1.44590e-06.
warning 3: [planner_server-8] W0323 14:01:33.240247 22652 line_search_direction.cc:86] Restarting non-linear conjugate gradients: 3.00844
*/

namespace smac_planner
{

/**
 * @struct smac_planner::CurvatureComputations
 * @brief Cache common computations between the curvature terms to minimize recomputations
 */
struct CurvatureComputations 
{
  /**
   * @brief A constructor for smac_planner::CurvatureComputations
   */
  CurvatureComputations() {
    valid = true;
  }

  bool valid;
  /**
   * @brief Check if result is valid for penalty
   * @return is valid (non-nan, non-inf, and turning angle > max)
   */
  bool isValid() {
    return valid;
  }

  Eigen::Vector2d delta_xi;
  Eigen::Vector2d delta_xi_p;
  double delta_xi_norm;
  double delta_xi_p_norm;
  double delta_phi_i;
  double turning_rad;
  double ki_minus_kmax;
};

/**
 * @struct smac_planner::UnconstrainedSmootherCostFunction
 * @brief Cost function for path smoothing with multiple terms
 * including curvature, smoothness, collision, and avoid obstacles.
 */
class UnconstrainedSmootherCostFunction : public ceres::FirstOrderFunction {
 public:
  /**
   * @brief A constructor for smac_planner::UnconstrainedSmootherCostFunction
   * @param num_points Number of path points to consider
   * @param costmap A minimal costmap wrapper to get values for collision and obstacle avoidance
   */
  UnconstrainedSmootherCostFunction(const int & num_points, MinimalCostmap * costmap)
  : _num_params(2 * num_points), _costmap(costmap)
  {
    // help normalize this more
    _Wsmooth = 200000.0;
    _Wcurve = 2.0;
    _Wcollision = 10.0;
    _max_turning_radius = 10.0; // 5.0 is pretty smooth yo
  }

  /**
   * @brief Smoother cost function evaluation
   * @param parameters X,Y pairs of points 
   * @param cost total cost of path
   * @param gradient of path at each X,Y pair from cost function derived analytically
   * @return if successful in computing values
   */
  virtual bool Evaluate(const double * parameters,
                        double * cost,
                        double * gradient) const {

    Eigen::Vector2d xi;
    Eigen::Vector2d xi_p1;
    Eigen::Vector2d xi_m1;
    uint x_index, y_index;
    cost[0] = 0.0;
    double cost_raw = 0.0;
    double grad_x_raw = 0.0;
    double grad_y_raw = 0.0;
    unsigned int mx, my;
    bool valid_coords = true;
    double costmap_cost = 0.0;

    // cache some computations between the residual and jacobian
    CurvatureComputations curvature_params;

    for (uint i = 0; i != NumParameters() / 2; i++) {
      x_index = 2 * i;
      y_index = 2 * i + 1;
      if (i < 1 || i >= NumParameters() / 2 - 1) {
        continue; 
      }

      xi = Eigen::Vector2d(parameters[x_index], parameters[y_index]);
      xi_p1 = Eigen::Vector2d(parameters[x_index + 2], parameters[y_index + 2]);
      xi_m1 = Eigen::Vector2d(parameters[x_index - 2], parameters[y_index - 2]);

      // compute cost
      addSmoothingResidual(_Wsmooth, xi, xi_p1, xi_m1, cost_raw);
      addMaxCurvatureResidual(_Wcurve, xi, xi_p1, xi_m1, curvature_params, cost_raw);

      if (valid_coords = _costmap->worldToMap(xi[0], xi[1], mx, my)) {
        costmap_cost = _costmap->getCost(mx, my);
        addCollisionResidual(_Wcollision, costmap_cost, cost_raw);
      }

    if (gradient != NULL) {
        // compute gradient
        gradient[x_index] = 0.0;
        gradient[y_index] = 0.0;
        addSmoothingJacobian(_Wsmooth, xi, xi_p1, xi_m1, grad_x_raw, grad_y_raw);
        addMaxCurvatureJacobian(_Wcurve, xi, xi_p1, xi_m1, curvature_params, grad_x_raw, grad_y_raw);

        if (valid_coords) {
          addCollisionJacobian(_Wcollision, mx, my, costmap_cost, grad_x_raw, grad_y_raw);          
        }

        gradient[x_index] = grad_x_raw;
        gradient[y_index] = grad_y_raw;
      }
    }

    cost[0] = cost_raw;
    std::cout << "Cost: " << cost[0] << " Cost Raw " << cost_raw << std::endl;

    return true;
  }

  /**
   * @brief Get number of parameter blocks
   * @return Number of parameters in cost function
   */
  virtual int NumParameters() const { return _num_params; }

  /**
   * @brief Cost function term for smooth paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param r Residual (cost) of term
   */
  inline void addSmoothingResidual(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    double & r) const
  {
    r += weight * (
      pt_p.dot(pt_p)
      - 4 * pt_p.dot(pt)
      + 2 * pt_p.dot(pt_m)
      + 4 * pt.dot(pt)
      - 4 * pt.dot(pt_m)
      + pt_m.dot(pt_m));  // objective function value
  }

  /**
   * @brief Cost function derivative term for smooth paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addSmoothingJacobian(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    double & j0,
    double & j1) const
  {
    j0 += weight * (- 4 * pt_m[0] + 8 * pt[0] - 4 * pt_p[0]);  // xi x component of partial-derivative
    j1 += weight * (- 4 * pt_m[1] + 8 * pt[1] - 4 * pt_p[1]);  // xi y component of partial-derivative
  }

  /**
   * @brief Cost function term for maximum curved paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param curvature_params A struct to cache computations for the jacobian to use
   * @param r Residual (cost) of term
   */
  inline void addMaxCurvatureResidual(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    CurvatureComputations & curvature_params,
    double & r) const
  {
    curvature_params.delta_xi = Eigen::Vector2d(pt[0] - pt_m[0], pt[1] - pt_m[1]);
    curvature_params.delta_xi_p = Eigen::Vector2d(pt_p[0] - pt[0], pt_p[1] - pt[1]); 
    curvature_params.delta_xi_norm = curvature_params.delta_xi.norm();
    curvature_params.delta_xi_p_norm = curvature_params.delta_xi_p.norm();

    if (curvature_params.delta_xi_norm < EPSILON || curvature_params.delta_xi_p_norm < EPSILON || 
      std::isnan(curvature_params.delta_xi_p_norm) || std::isnan(curvature_params.delta_xi_norm) ||
      std::isinf(curvature_params.delta_xi_p_norm) || std::isinf(curvature_params.delta_xi_norm)) {
      // ensure we have non-nan values returned
      curvature_params.valid = false;
      return;
    }

    const double & delta_xi_by_xi_p = curvature_params.delta_xi_norm * curvature_params.delta_xi_p_norm;
    double projection = curvature_params.delta_xi.dot(curvature_params.delta_xi_p) / delta_xi_by_xi_p;
    if (fabs(1 - projection) < EPSILON || fabs(projection + 1) < EPSILON) {
      projection = 1.0;
    }

    curvature_params.delta_phi_i = acos(projection);
    curvature_params.turning_rad = curvature_params.delta_phi_i / curvature_params.delta_xi_norm;

    curvature_params.ki_minus_kmax = curvature_params.turning_rad - _max_turning_radius;

    if (curvature_params.ki_minus_kmax <= EPSILON) {
      // Quadratic penalty need not apply
      curvature_params.valid = false;
      return;
    }

    r += weight * curvature_params.ki_minus_kmax * curvature_params.ki_minus_kmax;  // objective function value
  }

  /**
   * @brief Cost function derivative term for maximum curvature paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param curvature_params A struct with cached values to speed up Jacobian computation
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addMaxCurvatureJacobian(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    CurvatureComputations & curvature_params,
    double & j0,
    double & j1) const
  {
    if (!curvature_params.isValid()) {
      return;
    }

    const double & partial_delta_phi_i_wrt_cost_delta_phi_i = -1 / std::sqrt(1 - std::pow(std::cos(curvature_params.delta_phi_i), 2));
    const Eigen::Vector2d ones = Eigen::Vector2d(1.0, 1.0);
    auto neg_pt_plus = -1 * pt_p;
    Eigen::Vector2d p1 = normalizedOrthogonalComplement(pt, neg_pt_plus, curvature_params.delta_xi_norm, curvature_params.delta_xi_p_norm);
    Eigen::Vector2d p2 = normalizedOrthogonalComplement(neg_pt_plus, pt, curvature_params.delta_xi_norm, curvature_params.delta_xi_p_norm);

    const double & u = 2 * curvature_params.ki_minus_kmax;
    const double & common_prefix = (-1 / curvature_params.delta_xi_norm) * partial_delta_phi_i_wrt_cost_delta_phi_i;
    const double & common_suffix = curvature_params.delta_phi_i / (curvature_params.delta_xi_norm * curvature_params.delta_xi_norm);

    const Eigen::Vector2d jacobian = u * (common_prefix * (-p1 - p2) - (common_suffix * ones));
    j0 += weight * jacobian[0];  // xi x component of partial-derivative
    j1 += weight * jacobian[1];  // xi y component of partial-derivative

    // std::cout << std::endl;
    // std::cout << "pt_m " << pt_m[0] << " " << pt_m[1] << std::endl;
    // std::cout << "Delta Xi " << delta_xi[0] << " " << delta_xi[1] << std::endl;
    // std::cout << "Delta Xi_p " << delta_xi_p[0] << " " << delta_xi_p[1] << std::endl;
    // std::cout << "Delta Phi: " << delta_phi_i << " turn rad: " << turning_rad << " Max: " << max_turning_radius << std::endl;
    // std::cout << "NormXi: " << delta_xi_norm << " NormXi+1: " << delta_xi_p_norm  << std::endl;
    // std::cout << "pt: " << pt[0] <<  " " << pt[1] << std::endl;
    // std::cout << "Neg P1+1: " << neg_pt_plus[0] <<  " " << neg_pt_plus[1] << std::endl;
    // std::cout << "PDE: " << partial_delta_phi_i_wrt_cost_delta_phi_i << std::endl;
    // std::cout << "P1: " << p1[0] <<  " " << p1[1] << std::endl;
    // std::cout << "P2: " << p2[0] <<  " " << p2[1] << std::endl;
    // std::cout << "U: " << u << std::endl;
    // std::cout << "common_prefix: " << common_prefix << std::endl;
    // std::cout << "common_suffix: " << common_suffix << std::endl;
    // std::cout << "J: " << j0 << " " << j1 << std::endl;
    // r = weight * (curvature_params.turning_rad - max_turning_radius) * (curvature_params.turning_rad - max_turning_radius);
    // std::cout << "R: " << r << std::endl;
  }

  /**
   * @brief Cost function term for no collisions paths
   * @param weight Weight to apply to function
   * @param value Point Xi's cost'
   * @param r Residual (cost) of term
   */
  inline void addCollisionResidual(
    const double & weight,
    const double & value,
    double & r) const
  {
    if (value < INSCRIBED) {
      return;
    }

    // cost is a good approximation for distance since there's a defined relationship
    r += weight * (value * value - 2 * MAX_NON_OBSTACLE * value + MAX_NON_OBSTACLE * MAX_NON_OBSTACLE);  // objective function value
  }

  /**
   * @brief Cost function derivative term for no collisions paths
   * @param weight Weight to apply to function
   * @param mx Point Xi's x coordinate in map frame
   * @param mx Point Xi's y coordinate in map frame
   * @param value Point Xi's cost'
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addCollisionJacobian(
    const double & weight,
    const unsigned int & mx,
    const unsigned int & my,
    const double & value,
    double & j0,
    double & j1) const
  {
    if (value < INSCRIBED) {
      return;
    }

    double left_one = 0.0;
    double left_two = 0.0;
    double right_one = 0.0;
    double right_two = 0.0;
    double up_one = 0.0;
    double up_two = 0.0;
    double down_one = 0.0;
    double down_two = 0.0;

    if (mx < _costmap->sizeX()) {
        right_one = _costmap->getCost(mx + 1, my);
    }
    if (mx + 1 < _costmap->sizeX()) {
        right_two = _costmap->getCost(mx + 2, my);
    }
    if (mx > 0) {
        left_one = _costmap->getCost(mx - 1, my);
    }
    if (mx - 1 > 0) {
        left_two = _costmap->getCost(mx - 2, my);
    }
    if (my < _costmap->sizeY()) {
        up_one = _costmap->getCost(mx, my + 1);
    }
    if (my + 1 < _costmap->sizeY()) {
        up_two = _costmap->getCost(mx, my + 2);
    }
    if (my > 0) {
        down_one = _costmap->getCost(mx, my - 1);
    }
    if (my - 1 > 0) {
        left_two = _costmap->getCost(mx, my - 2);
    }

    // find unit vector that describes that direction
    // via 5 point taylor series approximation for gradient at Xi
    double gradx = (8.0 * up_one - up_two - 8.0 * down_one + down_two) / 12;
    double grady = (8.0 * right_one - right_two - 8.0 * left_one + left_two) / 12;
    const double grad_mag = hypot(gradx, grady);
    gradx /= grad_mag;
    grady /= grad_mag;

    const double & common_prefix = 2 * weight * (value - MAX_NON_OBSTACLE);

    // cost is a good approximation for distance since there's a defined relationship
    j0 += weight * common_prefix * gradx;  // xi x component of partial-derivative
    j1 += weight * common_prefix * grady;  // xi y component of partial-derivative
  }

protected:
  /**
   * @brief Computing the normalized orthogonal component of 2 vectors
   * @param a Vector
   * @param b Vector
   * @param norm a Vector's norm
   * @param norm b Vector's norm
   * @return Normalized vector of orthogonal components
   */
  inline Eigen::Vector2d normalizedOrthogonalComplement(
    const Eigen::Vector2d & a,
    const Eigen::Vector2d & b,
    const double & a_norm,
    const double & b_norm) const
  {
    return (a - (b * a.dot(b) / b.squaredNorm())) / (a_norm * b_norm);
  }

  int _num_params;
  double _Wsmooth;
  double _Wcurve;
  double _Wcollision;
  double _max_turning_radius;
  MinimalCostmap * _costmap;
};

}  // namespace smac_planner

#endif  // SMAC_PLANNER__SMOOTHER_COST_FUNCTION_HPP_




    // const Eigen::Vector2d jacobian = u * (common_prefix * (-p1 - p2) - (common_suffix * ones));
    // j0 += weight * jacobian[0];
    // j1 += weight * jacobian[1];
    // const Eigen::Vector2d jacobian_i_m = u * (common_prefix * p2 - (common_suffix * ones));
    // const Eigen::Vector2d jacobian_i_p = u * common_prefix * p1;

    // j0 += weight * (0.25 * jacobian_i_m[0] + 0.5 * jacobian_i[0] + 0.25 * jacobian_i_p[0]);  // xi x component of partial-derivative
    // j1 += weight * (0.25 * jacobian_i_m[1] + 0.5 * jacobian_i[1] + 0.25 * jacobian_i_p[1]);  // xi y component of partial-derivative


  // inline void addSmoothingResidual(const double & weight, const double & pt_x, const double & pt_y, double & r0, double & r1) const
  // {
  //   r0 +=
  //     weight * (pt_plus->first * pt_plus->first -
  //     4 * pt_plus->first * pt_x +
  //     2 * pt_plus->first * pt_minus->first +
  //     4 * pt_x * pt_x -
  //     4 * pt_x * pt_minus->first +
  //     pt_minus->first * pt_minus->first);  // objective function value x
  //   r1 +=
  //     weight * (pt_plus->second * pt_plus->second -
  //     4 * pt_plus->second * pt_y +
  //     2 * pt_plus->second * pt_minus->second +
  //     4 * pt_y * pt_y -
  //     4 * pt_y * pt_minus->second +
  //     pt_minus->second * pt_minus->second);  // objective function value y
  // }

  // inline void addSmoothingJacobian(const double & weight, const double & pt_x, const double & pt_y, double & j0, double & j1) const
  // {
  //   j0 += weight * (/*pt_minus_two.first*/ - 4 * pt_minus->first + 8 * pt_x - 4 * pt_plus->first /*+ pt_plus_two.first*/);  // x derivative
  //   j1 += weight * (/*pt_minus_two.second*/ - 4 * pt_minus->second + 8 * pt_y - 4 * pt_plus->second /*+ pt_plus_two.second*/);  // y derivative
  // }
