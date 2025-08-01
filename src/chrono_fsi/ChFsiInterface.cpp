// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2024 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Author: Radu Serban
// =============================================================================
//
// 1. Definition of the base class for interfacing between a Chrono system and
//    an FSI-aware fluid system.
// 2. Implementation of a generic FSI interface that relies on copying data to
//    intermediate buffers.
//
// =============================================================================

//// TODO: check if it is possible to use a more efficient implementation based on Thrust (with OpenMP backend)

////#define DEBUG_LOG

#include <vector>
#include <stdexcept>
#include <algorithm>

#include "chrono/utils/ChUtils.h"

#include "chrono_fsi/ChFsiInterface.h"

using std::cout;
using std::cerr;
using std::endl;

namespace chrono {
namespace fsi {

// =============================================================================

ChFsiInterface::ChFsiInterface(ChSystem& sysMBS, ChFsiFluidSystem& sysCFD)
    : m_sysMBS(sysMBS), m_sysCFD(sysCFD), m_verbose(true), m_initialized(false), m_use_node_directions(false) {}

ChFsiInterface::~ChFsiInterface() {}

// ------------

unsigned int ChFsiInterface::GetNumBodies() const {
    return (unsigned int)m_fsi_bodies.size();
}

unsigned int ChFsiInterface::GetNumMeshes1D() const {
    return (unsigned int)m_fsi_meshes1D.size();
}

unsigned int ChFsiInterface::GetNumMeshes2D() const {
    return (unsigned int)m_fsi_meshes2D.size();
}

unsigned int ChFsiInterface::GetNumElements1D() const {
    unsigned int n = 0;
    for (const auto& m : m_fsi_meshes1D)
        n += m->GetNumElements();
    return n;
}

unsigned int ChFsiInterface::GetNumNodes1D() const {
    unsigned int n = 0;
    for (const auto& m : m_fsi_meshes1D)
        n += m->GetNumNodes();
    return n;
}

unsigned int ChFsiInterface::GetNumElements2D() const {
    unsigned int n = 0;
    for (const auto& m : m_fsi_meshes2D)
        n += m->GetNumElements();
    return n;
}

unsigned int ChFsiInterface::GetNumNodes2D() const {
    unsigned int n = 0;
    for (const auto& m : m_fsi_meshes2D)
        n += m->GetNumNodes();
    return n;
}

// ------------

const ChVector3d& ChFsiInterface::GetFsiBodyForce(size_t i) const {
    return m_fsi_bodies[i]->fsi_force;
}

const ChVector3d& ChFsiInterface::GetFsiBodyTorque(size_t i) const {
    return m_fsi_bodies[i]->fsi_torque;
}

// ------------

std::shared_ptr<FsiBody> ChFsiInterface::AddFsiBody(std::shared_ptr<ChBody> body,
                                                    std::shared_ptr<utils::ChBodyGeometry> geometry,
                                                    bool check_embedded) {
    auto fsi_body = chrono_types::make_shared<FsiBody>();
    fsi_body->body = body;
    fsi_body->geometry = geometry;
    fsi_body->fsi_force = VNULL;
    fsi_body->fsi_torque = VNULL;

    // Create a force accumulator dedicated to FSI fluid forces
    fsi_body->fsi_accumulator = fsi_body->body->AddAccumulator();

    // Set the body index in list of FSI bodies
    fsi_body->index = m_fsi_bodies.size();

    // Let the fluid solver process the FSI solid
    m_sysCFD.OnAddFsiBody(fsi_body, check_embedded);

    // Store the body
    m_fsi_bodies.push_back(fsi_body);

    return m_fsi_bodies.back();
}

std::shared_ptr<FsiMesh1D> ChFsiInterface::AddFsiMesh1D(std::shared_ptr<fea::ChContactSurfaceSegmentSet> surface,
                                                        bool check_embedded) {
    auto fsi_mesh = chrono_types::make_shared<FsiMesh1D>();
    fsi_mesh->contact_surface = surface;

    // Create maps from pointer-based to index-based for the nodes in the mesh contact segments.
    // These maps index only the nodes that are in ANCF cable elements (and not all nodes in the given FEA mesh).
    int vertex_index = 0;
    for (const auto& seg : surface->GetSegmentsXYZ()) {
        if (fsi_mesh->ptr2ind_map.insert({seg->GetNode(0), vertex_index}).second) {
            fsi_mesh->ind2ptr_map.insert({vertex_index, seg->GetNode(0)});
            ++vertex_index;
        }
        if (fsi_mesh->ptr2ind_map.insert({seg->GetNode(1), vertex_index}).second) {
            fsi_mesh->ind2ptr_map.insert({vertex_index, seg->GetNode(1)});
            ++vertex_index;
        }
    }

    // Set the mesh index in list of FSI 1D meshes
    fsi_mesh->index = m_fsi_bodies.size();

    // Let the fluid solver process the FSI solid
    m_sysCFD.OnAddFsiMesh1D(fsi_mesh, check_embedded);

    // Store the mesh contact surface
    m_fsi_meshes1D.push_back(fsi_mesh);

    return m_fsi_meshes1D.back();
}

std::shared_ptr<FsiMesh2D> ChFsiInterface::AddFsiMesh2D(std::shared_ptr<fea::ChContactSurfaceMesh> surface,
                                                        bool check_embedded) {
    auto fsi_mesh = chrono_types::make_shared<FsiMesh2D>();
    fsi_mesh->contact_surface = surface;

    // Create maps from pointer-based to index-based for the nodes in the mesh contact surface.
    // These maps index only the nodes that are in the contact surface (and not all nodes in the given FEA mesh).
    int vertex_index = 0;
    for (const auto& tri : surface->GetTrianglesXYZ()) {
        if (fsi_mesh->ptr2ind_map.insert({tri->GetNode(0), vertex_index}).second) {
            fsi_mesh->ind2ptr_map.insert({vertex_index, tri->GetNode(0)});
            ++vertex_index;
        }
        if (fsi_mesh->ptr2ind_map.insert({tri->GetNode(1), vertex_index}).second) {
            fsi_mesh->ind2ptr_map.insert({vertex_index, tri->GetNode(1)});
            ++vertex_index;
        }
        if (fsi_mesh->ptr2ind_map.insert({tri->GetNode(2), vertex_index}).second) {
            fsi_mesh->ind2ptr_map.insert({vertex_index, tri->GetNode(2)});
            ++vertex_index;
        }
    }

    assert(fsi_mesh->ptr2ind_map.size() == surface->GetNumVertices());
    assert(fsi_mesh->ind2ptr_map.size() == surface->GetNumVertices());

    // Set the mesh index in list of FSI 1D meshes
    fsi_mesh->index = m_fsi_bodies.size();

    // Let the fluid solver process the FSI solid
    m_sysCFD.OnAddFsiMesh2D(fsi_mesh, check_embedded);

    // Store the mesh contact surface
    m_fsi_meshes2D.push_back(fsi_mesh);

    return m_fsi_meshes2D.back();
}

// ------------

void ChFsiInterface::Initialize() {
    if (m_verbose) {
        cout << "FSI interface solids" << endl;
        cout << "  Num. bodies:     " << GetNumBodies() << endl;
        cout << "  Num meshes 1D:   " << GetNumMeshes1D() << endl;
        cout << "  Num nodes 1D:    " << GetNumNodes1D() << endl;
        cout << "  Num elements 1D: " << GetNumElements1D() << endl;
        cout << "  Num meshes 2D:   " << GetNumMeshes2D() << endl;
        cout << "  Num nodes 2D:    " << GetNumNodes2D() << endl;
        cout << "  Num elements 2D: " << GetNumElements2D() << endl;
    }

    m_initialized = true;
}

// ------------

void ChFsiInterface::UseNodeDirections(bool val) {
    ChAssertAlways(!m_initialized);
    m_use_node_directions = val;
}

void ChFsiInterface::AllocateStateVectors(std::vector<FsiBodyState>& body_states,
                                          std::vector<FsiMeshState>& mesh1D_states,
                                          std::vector<FsiMeshState>& mesh2D_states) const {
    unsigned int num_bodies = GetNumBodies();
    unsigned int num_meshes1D = GetNumMeshes1D();
    unsigned int num_meshes2D = GetNumMeshes2D();

    body_states.resize(num_bodies);

    mesh1D_states.resize(num_meshes1D);
    for (unsigned int imesh = 0; imesh < num_meshes1D; imesh++) {
        unsigned int num_nodes = m_fsi_meshes1D[imesh]->GetNumNodes();
        mesh1D_states[imesh].pos.resize(num_nodes);
        mesh1D_states[imesh].vel.resize(num_nodes);
        mesh1D_states[imesh].acc.resize(num_nodes);
        if (m_use_node_directions)
            mesh1D_states[imesh].dir.resize(num_nodes);
    }

    mesh2D_states.resize(num_meshes2D);
    for (unsigned int imesh = 0; imesh < num_meshes2D; imesh++) {
        unsigned int num_nodes = m_fsi_meshes2D[imesh]->GetNumNodes();
        mesh2D_states[imesh].pos.resize(num_nodes);
        mesh2D_states[imesh].vel.resize(num_nodes);
        mesh2D_states[imesh].acc.resize(num_nodes);
        if (m_use_node_directions)
            mesh2D_states[imesh].dir.resize(num_nodes);
    }
}

bool ChFsiInterface::CheckStateVectors(const std::vector<FsiBodyState>& body_states,
                                       const std::vector<FsiMeshState>& mesh1D_states,
                                       const std::vector<FsiMeshState>& mesh2D_states) const {
    if (body_states.size() != m_fsi_bodies.size()) {
        cerr << "ERROR (ChFsiInterface::CheckStateVectors) incorrect size for vector of body states.";
        return false;
    }

    if (mesh1D_states.size() != m_fsi_meshes1D.size()) {
        cerr << "ERROR (ChFsiInterface::CheckStateVectors) incorrect size for vector of mesh node states.";
        return false;
    }

    for (size_t i = 0; i < mesh1D_states.size(); i++) {
        auto num_nodes = m_fsi_meshes1D[i]->GetNumNodes();
        if (mesh1D_states[i].pos.size() != num_nodes ||  //
            mesh1D_states[i].vel.size() != num_nodes ||  //
            mesh1D_states[i].acc.size() != num_nodes) {
            cerr << "ERROR (ChFsiInterface::CheckStateVectors) incorrect size of state vectors for mesh1D #" << i
                 << endl;
            return false;
        }
    }

    if (mesh2D_states.size() != m_fsi_meshes2D.size()) {
        cerr << "ERROR (ChFsiInterface::CheckStateVectors) incorrect size for vector of mesh node states.";
        return false;
    }

    for (size_t i = 0; i < mesh2D_states.size(); i++) {
        auto num_nodes = m_fsi_meshes2D[i]->GetNumNodes();
        if (mesh2D_states[i].pos.size() != num_nodes ||  //
            mesh2D_states[i].vel.size() != num_nodes ||  //
            mesh2D_states[i].acc.size() != num_nodes) {
            cerr << "ERROR (ChFsiInterface::CheckStateVectors) incorrect size of state vectors for mesh2D #" << i
                 << endl;
            return false;
        }
    }

    return true;
}

// Options for nodal direction calculation
// NODAL_DIR_METHOD = 1:  average over adjacent elements
// NODAL_DIR_METHOD = 2:  normalized sum over adjacent elements

#define NODAL_DIR_METHOD 1

// Utility function to calculate direction vectors at the flexible 1-D mesh nodes.
// For 1-D meshes, these are averages of the segment direction vectors of adjacent segments.
void CalculateDirectionsMesh1D(const FsiMesh1D& mesh, FsiMeshState& states) {
    std::vector<int> counts(mesh.GetNumNodes(), 0);
    std::fill(states.dir.begin(), states.dir.end(), VNULL);

    for (const auto& seg : mesh.contact_surface->GetSegmentsXYZ()) {
        auto i0 = mesh.ptr2ind_map.at(seg->GetNode(0));
        auto i1 = mesh.ptr2ind_map.at(seg->GetNode(1));
        const auto& P0 = states.pos[i0];
        const auto& P1 = states.pos[i1];
        auto d = P1 - P0;
#if NODAL_DIR_METHOD == 2
        d.Normalize();
#endif
        states.dir[i0] += d;
        states.dir[i1] += d;
        counts[i0]++;
        counts[i1]++;
    }

#if NODAL_DIR_METHOD == 1
    std::transform(states.dir.begin(), states.dir.end(), counts.begin(), states.dir.begin(),
                   [](const ChVector3d& v, int count) { return v / count; });
#elif NODAL_DIR_METHOD == 2
    // Normalize nodal directions
    for (auto& d : states.dir)
        d.Normalize();
#endif

#ifdef DEBUG_LOG
    for (auto& d : states.dir)
        cout << d << endl;
#endif
}

// Utility function to calculate direction vectors at the flexible 2-D mesh nodes.
// For 2-D meshes, these are averages of the face normals of adjacent faces.
void CalculateDirectionsMesh2D(const FsiMesh2D& mesh, FsiMeshState& states) {
    std::vector<int> counts(mesh.GetNumNodes(), 0);
    std::fill(states.dir.begin(), states.dir.end(), VNULL);

    for (const auto& tri : mesh.contact_surface->GetTrianglesXYZ()) {
        auto i0 = mesh.ptr2ind_map.at(tri->GetNode(0));
        auto i1 = mesh.ptr2ind_map.at(tri->GetNode(1));
        auto i2 = mesh.ptr2ind_map.at(tri->GetNode(2));
        const auto& P0 = states.pos[i0];
        const auto& P1 = states.pos[i1];
        const auto& P2 = states.pos[i2];
        auto d = Vcross(P1 - P0, P2 - P0);
#if NODAL_DIR_METHOD == 2
        d.Normalize();
#endif
        states.dir[i0] += d;
        states.dir[i1] += d;
        states.dir[i2] += d;
        counts[i0]++;
        counts[i1]++;
        counts[i2]++;
    }

#if NODAL_DIR_METHOD == 1
    std::transform(states.dir.begin(), states.dir.end(), counts.begin(), states.dir.begin(),
                   [](const ChVector3d& v, int count) { return v / count; });
#elif NODAL_DIR_METHOD == 2
    // Normalize nodal directions
    for (auto& d : states.dir)
        d.Normalize();
#endif
}

void ChFsiInterface::StoreSolidStates(std::vector<FsiBodyState>& body_states,
                                      std::vector<FsiMeshState>& mesh1D_states,
                                      std::vector<FsiMeshState>& mesh2D_states) {
    if (!CheckStateVectors(body_states, mesh1D_states, mesh2D_states)) {
        throw std::runtime_error("(ChFsiInterface::StoreSolidStates) incorrect state vector sizes.");
    }

    {
        size_t num_bodies = m_fsi_bodies.size();
        for (size_t ibody = 0; ibody < num_bodies; ibody++) {
            std::shared_ptr<ChBody> body = m_fsi_bodies[ibody]->body;

            body_states[ibody].pos = body->GetPos();
            body_states[ibody].lin_vel = body->GetPosDt();
            body_states[ibody].lin_acc = body->GetPosDt2();
            body_states[ibody].rot = body->GetRot();
            body_states[ibody].ang_vel = body->GetAngVelLocal();
            body_states[ibody].ang_acc = body->GetAngAccLocal();
        }
    }

    {
        int imesh = 0;
        for (const auto& fsi_mesh : m_fsi_meshes1D) {
            int num_nodes = (int)fsi_mesh->ind2ptr_map.size();
            for (int inode = 0; inode < num_nodes; inode++) {
                const auto& node = fsi_mesh->ind2ptr_map.at(inode);
                mesh1D_states[imesh].pos[inode] = node->GetPos();
                mesh1D_states[imesh].vel[inode] = node->GetPosDt();
                mesh1D_states[imesh].acc[inode] = node->GetPosDt2();
            }
            if (m_use_node_directions) {
                ChDebugLog("mesh1D " << imesh << " dir size: " << mesh1D_states[imesh].dir.size());
                CalculateDirectionsMesh1D(*fsi_mesh, mesh1D_states[imesh]);
                mesh1D_states[imesh].has_node_directions = true;
            }
            imesh++;
        }
    }

    {
        int imesh = 0;
        for (const auto& fsi_mesh : m_fsi_meshes2D) {
            int num_nodes = (int)fsi_mesh->ind2ptr_map.size();
            for (int inode = 0; inode < num_nodes; inode++) {
                const auto& node = fsi_mesh->ind2ptr_map.at(inode);
                mesh2D_states[imesh].pos[inode] = node->GetPos();
                mesh2D_states[imesh].vel[inode] = node->GetPosDt();
                mesh2D_states[imesh].acc[inode] = node->GetPosDt2();
            }
            if (m_use_node_directions) {
                ChDebugLog("mesh2D " << imesh << " dir size: " << mesh2D_states[imesh].dir.size());
                CalculateDirectionsMesh2D(*fsi_mesh, mesh2D_states[imesh]);
                mesh2D_states[imesh].has_node_directions = true;
            }
            imesh++;
        }
    }
}

void ChFsiInterface::AllocateForceVectors(std::vector<FsiBodyForce>& body_forces,
                                          std::vector<FsiMeshForce>& mesh_forces1D,
                                          std::vector<FsiMeshForce>& mesh_forces2D) const {
    unsigned int num_bodies = GetNumBodies();
    unsigned int num_meshes1D = GetNumMeshes1D();
    unsigned int num_meshes2D = GetNumMeshes2D();

    body_forces.resize(num_bodies);

    mesh_forces1D.resize(num_meshes1D);
    for (unsigned int imesh = 0; imesh < num_meshes1D; imesh++) {
        unsigned int num_nodes = m_fsi_meshes1D[imesh]->GetNumNodes();
        mesh_forces1D[imesh].force.resize(num_nodes);
    }

    mesh_forces2D.resize(num_meshes2D);
    for (unsigned int imesh = 0; imesh < num_meshes2D; imesh++) {
        unsigned int num_nodes = m_fsi_meshes2D[imesh]->GetNumNodes();
        mesh_forces2D[imesh].force.resize(num_nodes);
    }
}

bool ChFsiInterface::CheckForceVectors(const std::vector<FsiBodyForce>& body_forces,
                                       const std::vector<FsiMeshForce>& mesh_forces1D,
                                       const std::vector<FsiMeshForce>& mesh_forces2D) const {
    if (body_forces.size() != m_fsi_bodies.size()) {
        cerr << "ERROR (ChFsiInterface::CheckForceVectors) incorrect size for vector of body forces.";
        return false;
    }

    if (mesh_forces1D.size() != m_fsi_meshes1D.size()) {
        cerr << "ERROR (ChFsiInterface::CheckForceVectors) incorrect size for vector of mesh node forces.";
        return false;
    }

    for (size_t i = 0; i < mesh_forces1D.size(); i++) {
        auto num_nodes = m_fsi_meshes1D[i]->GetNumNodes();
        if (mesh_forces1D[i].force.size() != num_nodes) {
            cerr << "ERROR (ChFsiInterface::CheckForceVectors) incorrect size of force vectors for mesh1D #" << i
                 << endl;
            return false;
        }
    }

    if (mesh_forces2D.size() != m_fsi_meshes2D.size()) {
        cerr << "ERROR (ChFsiInterface::CheckForceVectors) incorrect size for vector of mesh node forces.";
        return false;
    }

    for (size_t i = 0; i < mesh_forces2D.size(); i++) {
        auto num_nodes = m_fsi_meshes2D[i]->GetNumNodes();
        if (mesh_forces2D[i].force.size() != num_nodes) {
            cerr << "ERROR (ChFsiInterface::CheckForceVectors) incorrect size of force vectors for mesh2D #" << i
                 << endl;
            return false;
        }
    }

    return true;
}

void ChFsiInterface::LoadSolidForces(std::vector<FsiBodyForce>& body_forces,
                                     std::vector<FsiMeshForce>& mesh1D_forces,
                                     std::vector<FsiMeshForce>& mesh2D_forces) {
    if (!CheckForceVectors(body_forces, mesh1D_forces, mesh2D_forces)) {
        throw std::runtime_error("(ChFsiInterface::LoadSolidForces) incorrect force vector sizes.");
    }

    // External loads on rigid bodies
    {
        size_t ibody = 0;
        for (const auto& fsi_body : m_fsi_bodies) {
            fsi_body->body->EmptyAccumulator(fsi_body->fsi_accumulator);
            fsi_body->body->AccumulateForce(fsi_body->fsi_accumulator, body_forces[ibody].force, fsi_body->body->GetPos(),
                                           false);
            fsi_body->body->AccumulateTorque(fsi_body->fsi_accumulator, body_forces[ibody].torque, false);
            ibody++;
        }
    }

    // External loads on FEA 1-D mesh nodes
    {
        size_t imesh = 0;
        for (const auto& fsi_mesh : m_fsi_meshes1D) {
            size_t inode = 0;
            for (auto& node : fsi_mesh->ind2ptr_map) {
                node.second->SetForce(mesh2D_forces[imesh].force[inode]);
                inode++;
            }
            imesh++;
        }
    }

    // External loads on FEA 2-D mesh nodes
    {
        size_t imesh = 0;
        for (const auto& fsi_mesh : m_fsi_meshes2D) {
            size_t inode = 0;
            for (auto& node : fsi_mesh->ind2ptr_map) {
                node.second->SetForce(mesh2D_forces[imesh].force[inode]);
                inode++;
            }
            imesh++;
        }
    }
}

// =============================================================================

ChFsiInterfaceGeneric::ChFsiInterfaceGeneric(ChSystem& sysMBS, ChFsiFluidSystem& sysCFD)
    : ChFsiInterface(sysMBS, sysCFD) {}

ChFsiInterfaceGeneric::~ChFsiInterfaceGeneric() {}

void ChFsiInterfaceGeneric::Initialize() {
    ChFsiInterface::Initialize();

    AllocateStateVectors(m_body_states, m_mesh1D_states, m_mesh2D_states);
    AllocateForceVectors(m_body_forces, m_mesh1D_forces, m_mesh2D_forces);
}

void ChFsiInterfaceGeneric::ExchangeSolidStates() {
    // Get solid states from multibody system in cached vectors
    StoreSolidStates(m_body_states, m_mesh1D_states, m_mesh2D_states);

    // Pass solid states to the fluid solver
    m_sysCFD.LoadSolidStates(m_body_states, m_mesh1D_states, m_mesh2D_states);
}

void ChFsiInterfaceGeneric::ExchangeSolidForces() {
    // Get solid forces from the fluid solver
    m_sysCFD.StoreSolidForces(m_body_forces, m_mesh1D_forces, m_mesh2D_forces);

    // Load solid forces to the multibody system (apply as external loads)
    LoadSolidForces(m_body_forces, m_mesh1D_forces, m_mesh2D_forces);
}

// =============================================================================

}  // end namespace fsi
}  // end namespace chrono
