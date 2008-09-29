/* -------------------------------------------------------------------------- *
 *                      SimTK Core: SimTK Simbody(tm)                         *
 * -------------------------------------------------------------------------- *
 * This is part of the SimTK Core biosimulation toolkit originating from      *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2008 Stanford University and the Authors.           *
 * Authors: Peter Eastman                                                     *
 * Contributors:                                                              *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */

#include "SimTKcommon.h"

#include "simbody/internal/common.h"
#include "simbody/internal/Contact.h"
#include "simbody/internal/ContactGeometryImpl.h"
#include "simbody/internal/GeneralContactSubsystem.h"
#include "simbody/internal/MobilizedBody.h"
#include "ElasticFoundationForceImpl.h"
#include <map>
#include <set>

using std::map;
using std::set;
using std::vector;

namespace SimTK {

SimTK_INSERT_DERIVED_HANDLE_DEFINITIONS(ElasticFoundationForce, ElasticFoundationForceImpl, Force);

ElasticFoundationForce::ElasticFoundationForce(GeneralForceSubsystem& forces, GeneralContactSubsystem& contacts, ContactSetIndex set) :
        Force(new ElasticFoundationForceImpl(contacts, set)) {
    updImpl().setForceIndex(forces.adoptForce(*this));
}

void ElasticFoundationForce::setBodyParameters(int bodyIndex, Real stiffness, Real dissipation, Real staticFriction, Real dynamicFriction, Real viscousFriction) {
    updImpl().setBodyParameters(bodyIndex, stiffness, dissipation, staticFriction, dynamicFriction, viscousFriction);
}

Real ElasticFoundationForce::getTransitionVelocity() const {
    return getImpl().transitionVelocity;
}

void ElasticFoundationForce::setTransitionVelocity(Real v) {
    updImpl().transitionVelocity = v;
}

ElasticFoundationForceImpl::ElasticFoundationForceImpl(GeneralContactSubsystem& subsystem, ContactSetIndex set) : 
        subsystem(subsystem), set(set), transitionVelocity(0.01), energyCacheIndex(-1) {
}

void ElasticFoundationForceImpl::setBodyParameters(int bodyIndex, Real stiffness, Real dissipation, Real staticFriction, Real dynamicFriction, Real viscousFriction) {
    SimTK_APIARGCHECK1(bodyIndex >= 0 && bodyIndex < subsystem.getNumBodies(set), "ElasticFoundationForceImpl", "setBodyParameters",
            "Illegal body index: %d", bodyIndex);
    SimTK_APIARGCHECK1(subsystem.getBodyGeometry(set, bodyIndex).getType() == ContactGeometry::TriangleMeshImpl::Type(), "ElasticFoundationForceImpl", "setBodyParameters",
            "Body %d is not a triangle mesh", bodyIndex);
    parameters[bodyIndex] = Parameters(stiffness, dissipation, staticFriction, dynamicFriction, viscousFriction);
    const ContactGeometry::TriangleMesh& mesh = static_cast<const ContactGeometry::TriangleMesh&>(subsystem.getBodyGeometry(set, bodyIndex));
    Parameters& param = parameters[bodyIndex];
    param.springPosition.resize(mesh.getNumFaces());
    param.springNormal.resize(mesh.getNumFaces());
    param.springArea.resize(mesh.getNumFaces());
    Vec2 uv(1.0/3.0, 1.0/3.0);
    for (int i = 0; i < param.springPosition.size(); i++) {
        param.springPosition[i] = (mesh.getVertexPosition(mesh.getFaceVertex(i, 0))+mesh.getVertexPosition(mesh.getFaceVertex(i, 1))+mesh.getVertexPosition(mesh.getFaceVertex(i, 2)))/3.0;
        param.springNormal[i] = -mesh.findNormalAtPoint(i, uv);
        param.springArea[i] = mesh.getFaceArea(i);
    }
    subsystem.invalidateSubsystemTopologyCache();
}

void ElasticFoundationForceImpl::calcForce(const State& state, Vector_<SpatialVec>& bodyForces, Vector_<Vec3>& particleForces, Vector& mobilityForces) const {
    const vector<Contact>& contacts = subsystem.getContacts(state, set);
    Real& pe = Value<Real>::downcast(state.updCacheEntry(subsystem.getMySubsystemIndex(), energyCacheIndex)).upd();
    pe = 0.0;
    for (int i = 0; i < contacts.size(); i++) {
        map<int, Parameters>::const_iterator iter = parameters.find(contacts[i].getFirstBody());
        if (iter != parameters.end()) {
            const TriangleMeshContact& contact = static_cast<const TriangleMeshContact&>(contacts[i]);
            processContact(state, contact.getFirstBody(), contact.getSecondBody(), iter->second, contact.getFirstBodyFaces(), bodyForces, pe);
        }
        iter = parameters.find(contacts[i].getSecondBody());
        if (iter != parameters.end()) {
            const TriangleMeshContact& contact = static_cast<const TriangleMeshContact&>(contacts[i]);
            processContact(state, contact.getSecondBody(), contact.getFirstBody(), iter->second, contact.getSecondBodyFaces(), bodyForces, pe);
        }
    }
}

void ElasticFoundationForceImpl::processContact(const State& state, int meshIndex, int otherBodyIndex, const Parameters& param, const std::set<int>& insideFaces, Vector_<SpatialVec>& bodyForces, Real& pe) const {
    const ContactGeometry& otherObject = subsystem.getBodyGeometry(set, otherBodyIndex);
    const MobilizedBody& body1 = subsystem.getBody(set, meshIndex);
    const MobilizedBody& body2 = subsystem.getBody(set, otherBodyIndex);
    const Transform t1g = body1.getBodyTransform(state)*subsystem.getBodyTransform(set, meshIndex); // mesh to ground
    const Transform t2g = body2.getBodyTransform(state)*subsystem.getBodyTransform(set, otherBodyIndex); // other object to ground
    const Transform t12 = ~t2g*t1g; // mesh to other object

    // Loop over all the springs, and evaluate the force from each one.

    for (std::set<int>::const_iterator iter = insideFaces.begin(); iter != insideFaces.end(); ++iter) {
        int face = *iter;
        UnitVec3 normal;
        bool inside;
        Vec3 nearestPoint = otherObject.findNearestPoint(t12*param.springPosition[face], inside, normal);
        if (!inside)
            continue;
        
        // Find how much the spring is displaced.
        
        nearestPoint = t2g*nearestPoint;
        const Vec3 springPosInGround = t1g*param.springPosition[face];
        const Vec3 displacement = nearestPoint-springPosInGround;
        const Real distance = displacement.norm();
        if (distance == 0.0)
            continue;
        const Vec3 forceDir = displacement/distance;
        
        // Calculate the relative velocity of the two bodies at the contact point.
        
        const Vec3 station1 = body1.findStationAtGroundPoint(state, nearestPoint);
        const Vec3 station2 = body2.findStationAtGroundPoint(state, nearestPoint);
        const Vec3 v1 = body1.findStationVelocityInGround(state, station1);
        const Vec3 v2 = body2.findStationVelocityInGround(state, station2);
        const Vec3 v = v2-v1;
        const Real vnormal = dot(v, forceDir);
        const Vec3 vtangent = v-vnormal*forceDir;
        
        // Calculate the damping force.
        
        const Real f = param.stiffness*param.springArea[face]*(distance+param.dissipation*vnormal);
        Vec3 force = (f > 0 ? f*forceDir : Vec3(0));
        
        // Calculate the friction force.
        
        const Real vslip = vtangent.norm();
        if (f > 0 && vslip != 0) {
            const Real vrel = vslip/transitionVelocity;
            const Real ffriction = f*(std::min(vrel, 1.0)*(param.dynamicFriction+2*(param.staticFriction-param.dynamicFriction)/(1+vrel*vrel))+param.viscousFriction*vslip);
            force += ffriction*vtangent/vslip;
        }

        body1.applyForceToBodyPoint(state, station1, force, bodyForces);
        body2.applyForceToBodyPoint(state, station2, -force, bodyForces);
        pe += 0.5*param.stiffness*param.springArea[face]*displacement.normSqr();
    }
}

Real ElasticFoundationForceImpl::calcPotentialEnergy(const State& state) const {
    return Value<Real>::downcast(state.getCacheEntry(subsystem.getMySubsystemIndex(), energyCacheIndex)).get();
}

void ElasticFoundationForceImpl::realizeTopology(State& state) const {
        energyCacheIndex = state.allocateCacheEntry(subsystem.getMySubsystemIndex(), Stage::Dynamics, new Value<Real>());
}


} // namespace SimTK
