#include "floor_contact_force.h"
#include "rod_mechanics/elastic_joint.h"
#include "rod_mechanics/elastic_rod.h"
#include "rod_mechanics/external_forces/symbolic_equations.h"
#include "rod_mechanics/soft_robots.h"
#include "time_steppers/base_time_stepper.h"

/* TODO: CURRENTLY ASSUMES JOINTS DO NOT EXIST AS CONTACT POINTS
 * ADD CONTACT FOR JOINTS!!!
 * TODO: ADD MORE EFFICIENT COLLISION DETECTION
 */

// Floor friction defaults to the friction coefficient of the limb if floor mu
// is not specified
FloorContactForce::FloorContactForce(const std::shared_ptr<SoftRobots>& soft_robots,
                                     double floor_delta, double floor_slipTol, double floor_z,
                                     double floor_mu, double cylinder_radius_,
                                     const Vec3& cylinder_axis_)
    : BaseForce(soft_robots), delta(floor_delta), slipTol(floor_slipTol), floor_z(floor_z),
      floor_mu(floor_mu), cylinder_radius(cylinder_radius_), cylinder_axis(cylinder_axis_) {
    K1 = 15 / delta;
    K2 = 15 / slipTol;

    contact_stiffness = 1e4;

    contact_input[1] = K1;

    fric_jacobian_input[7] = K2;

    sym_eqs = std::make_shared<SymbolicEquations>();
    sym_eqs->generateFloorFrictionJacobianFunctions();
}

FloorContactForce::~FloorContactForce() {
    ;
}

void FloorContactForce::computeForce(double dt) {
    double dist;
    double f;
    int limb_idx = 0;
    int ind;
    Vec2 curr_node, pre_node;
    double v;  // exp(K1(floor_z - \Delta))
    min_dist = 1e7;
    num_contacts = 0;
    double cylinder_radius;  // Radius of the cylindrical floor
    Vec3 cylinder_axis;      // Unit vector along the central axis (e.g., (0, 0, 1) for z-axis)
    for (const auto& limb : soft_robots->limbs) {
        for (int i = 0; i < limb->nv; i++) {
            ind = 4 * i + 2;
            if (limb->isDOFJoint[ind] == 1)
                continue;

            // OLD VERSION dist = limb->x[ind] - limb->rod_radius - floor_z;

            // Get the vertex position
            Vec3 vertex_pos = limb->getVertex(i);

            // Project the vertex position onto the plane perpendicular to the cylinder axis
            Vec3 projected_pos = vertex_pos - (vertex_pos.dot(cylinder_axis)) * cylinder_axis;

            // Calculate the distance from the cylinder's axis
            double distance_from_axis = projected_pos.norm();

            // Distance to the cylinder surface
            dist = distance_from_axis - cylinder_radius - limb->rod_radius;

            if (dist < min_dist)
                min_dist = dist;
            if (dist > delta)
                continue;

            v = exp(-K1 * dist);

            f = (-2 * v * log(v + 1)) / (K1 * (v + 1));
            f *= contact_stiffness;

            // OLD VERSION stepper->addForce(ind, f, limb_idx);

            // Calculate the direction of the contact force (radial direction)
            Vec3 contact_force_direction = projected_pos.normalized();

            // Apply the force in the calculated direction (modified lines)
            stepper->addForce(4 * i, f * contact_force_direction(0), limb_idx);
            stepper->addForce(4 * i + 1, f * contact_force_direction(1), limb_idx);
            stepper->addForce(4 * i + 2, f * contact_force_direction(2), limb_idx);

            double mu = std::max(floor_mu, limb->mu);

            if (mu != 0) {
                curr_node = limb->getVertex(i)(Eigen::seq(0, 1));
                pre_node = limb->getPreVertex(i)(Eigen::seq(0, 1));
                computeFriction(curr_node, pre_node, f, mu, dt);
                stepper->addForce(4 * i, ffr(0), limb_idx);
                stepper->addForce(4 * i + 1, ffr(1), limb_idx);
            }

            num_contacts++;
        }
        limb_idx++;
    }
}

void FloorContactForce::computeForceAndJacobian(double dt) {
    double dist;
    double f;
    double J;
    int limb_idx = 0;
    int ind;
    Vec2 curr_node, pre_node;
    double v;                // exp(K1(floor_z - \Delta))
    double cylinder_radius;  // Radius of the cylindrical floor
    Vec3 cylinder_axis;      // Unit vector along the central axis (e.g., (0, 0, 1) for z-axis)
    min_dist = 1e7;
    num_contacts = 0;
    for (const auto& limb : soft_robots->limbs) {
        for (int i = 0; i < limb->nv; i++) {
            ind = 4 * i + 2;
            if (limb->isDOFJoint[ind] == 1)
                continue;

            // OLD VERSION dist = limb->x[ind] - limb->rod_radius - floor_z;
            // Get the vertex position
            Vec3 vertex_pos = limb->getVertex(i);

            // Project the vertex position onto the plane perpendicular to the cylinder axis
            Vec3 projected_pos = vertex_pos - (vertex_pos.dot(cylinder_axis)) * cylinder_axis;

            // Calculate the distance from the cylinder's axis
            double distance_from_axis = projected_pos.norm();

            // Distance to the cylinder surface
            dist = distance_from_axis - cylinder_radius - limb->rod_radius;

            if (dist < min_dist)
                min_dist = dist;
            if (dist > delta)
                continue;

            v = exp(-K1 * dist);

            f = (-2 * v * log(v + 1)) / (K1 * (v + 1));
            J = (2 * v * log(v + 1) + v) / pow(v + 1, 2);

            f *= contact_stiffness;
            J *= contact_stiffness;

            // OLD VERSION stepper->addForce(ind, f, limb_idx);
            // OLD VERSION stepper->addJacobian(ind, ind, J, limb_idx);

            // Calculate the direction of the contact force (radial direction)
            Vec3 contact_force_direction = projected_pos.normalized();

            // Apply the force in the calculated direction (modified lines)
            stepper->addForce(4 * i, f * contact_force_direction(0), limb_idx);
            stepper->addForce(4 * i + 1, f * contact_force_direction(1), limb_idx);
            stepper->addForce(4 * i + 2, f * contact_force_direction(2), limb_idx);

            // Calculate Jacobian for the radial force
            Vec3 dDistance_dVertex =
                projected_pos /
                distance_from_axis;  // Derivative of distance w.r.t. vertex position
            double df_dDistance = contact_stiffness * (2 * v * (log(v + 1) - 1) + v) /
                                  pow(v + 1, 2);  // Derivative of force w.r.t. distance
            Vec3 df_dVertex = df_dDistance * dDistance_dVertex;  // Chain rule

            // Add Jacobian components
            stepper->addJacobian(4 * i, 4 * i, df_dVertex(0), limb_idx);
            stepper->addJacobian(4 * i, 4 * i + 1, df_dVertex(1), limb_idx);
            stepper->addJacobian(4 * i, 4 * i + 2, df_dVertex(2), limb_idx);

            double mu = std::max(floor_mu, limb->mu);

            if (mu != 0.0) {
                curr_node = limb->getVertex(i)(Eigen::seq(0, 1));
                pre_node = limb->getPreVertex(i)(Eigen::seq(0, 1));
                computeFriction(curr_node, pre_node, f, mu, dt);

                if (fric_jaco_type == 0)
                    continue;

                stepper->addForce(4 * i, ffr(0), limb_idx);
                stepper->addForce(4 * i + 1, ffr(1), limb_idx);

                // Friction jacobian
                prepFrictionJacobianInput(curr_node, pre_node, f, mu, dt);

                if (fric_jaco_type == 1) {
                    sym_eqs->floor_friction_partials_gamma1_dfr_dx_func.call(
                        friction_partials_dfr_dx.data(), fric_jacobian_input.data());
                    sym_eqs->floor_friction_partials_gamma1_dfr_dfn_func.call(
                        friction_partials_dfr_dfn.data(), fric_jacobian_input.data());
                }
                else {
                    sym_eqs->floor_friction_partials_dfr_dx_func.call(
                        friction_partials_dfr_dx.data(), fric_jacobian_input.data());
                    sym_eqs->floor_friction_partials_dfr_dfn_func.call(
                        friction_partials_dfr_dfn.data(), fric_jacobian_input.data());
                }

                // dfrx/dx
                stepper->addJacobian(4 * i, 4 * i, friction_partials_dfr_dx(0, 0), limb_idx);
                stepper->addJacobian(4 * i, 4 * i + 1, friction_partials_dfr_dx(0, 1), limb_idx);
                // dfry/dy
                stepper->addJacobian(4 * i + 1, 4 * i, friction_partials_dfr_dx(1, 0), limb_idx);
                stepper->addJacobian(4 * i + 1, 4 * i + 1, friction_partials_dfr_dx(1, 1),
                                     limb_idx);
                // dfrx/dfn * dfn/dz
                stepper->addJacobian(4 * i + 2, 4 * i, friction_partials_dfr_dfn(0) * J, limb_idx);
                // dfry/dfn * dfn/dz
                stepper->addJacobian(4 * i + 2, 4 * i + 1, friction_partials_dfr_dfn(1) * J,
                                     limb_idx);
            }

            num_contacts++;
        }
        limb_idx++;
    }
}

void FloorContactForce::computeFriction(const Vec2& curr_node, const Vec2& pre_node, double fn,
                                        double mu, double dt) {
    Vec2 v, v_hat;
    double v_n, gamma;

    v = (curr_node - pre_node) / dt;
    v_n = v.norm();
    v_hat = v / v_n;

    if (v_n == 0) {
        fric_jaco_type = 0;
        ffr.setZero();
        return;
    }
    else if (v_n > slipTol) {
        fric_jaco_type = 1;
        gamma = 1;
    }
    else {
        fric_jaco_type = 2;
        gamma = 2 / (1 + exp(-K2 * v_n)) - 1;
    }
    //    std::cout << "friction gamma " << gamma << std::endl;
    ffr = -gamma * mu * fn * v_hat;
}

void FloorContactForce::prepFrictionJacobianInput(const Vec2& curr_node, const Vec2& pre_node,
                                                  double fn, double mu, double dt) {
    fric_jacobian_input(0) = curr_node(0);
    fric_jacobian_input(1) = curr_node(1);
    fric_jacobian_input(2) = pre_node(0);
    fric_jacobian_input(3) = pre_node(1);
    fric_jacobian_input(4) = fn;
    fric_jacobian_input[5] = mu;
    fric_jacobian_input[6] = dt;
}
