/*
   This file is part of HPDDM.

   Author(s): Pierre Jolivet <jolivet@ann.jussieu.fr>
        Date: 2012-10-04

   Copyright (C) 2011-2014 Université de Grenoble

   HPDDM is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published
   by the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   HPDDM is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with HPDDM.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _COARSE_OPERATOR_IMPL_
#define _COARSE_OPERATOR_IMPL_

#include "coarse_operator.hpp"
#ifdef __GNUG__
#include <cxxabi.h>
#include <memory>
#endif
#if HPDDM_OUTPUT_CO
#include <iterator>
#include <fstream>
#endif

namespace HPDDM {
#ifdef __GNUG__
std::string demangle(const char* name) {
    int status;
    std::unique_ptr<char, void(*)(void*)> res { abi::__cxa_demangle(name, NULL, NULL, &status), std::free };
    return status == 0 ? res.get() : name;
}
#else
std::string demangle(const char* name) {
    return name;
}
#endif
template<template<class> class Solver, char S, class K>
template<char T, bool exclude>
inline void CoarseOperator<Solver, S, K>::constructionCommunicator(const MPI_Comm& comm, unsigned short& p) {
    MPI_Comm_size(comm, &_sizeWorld);
    MPI_Comm_rank(comm, &_rankWorld);
    p = std::max(p, static_cast<unsigned short>(1));
    if(p >= _sizeWorld) {
        p = _sizeWorld / 2;
        if(_rankWorld == 0)
            std::cout << "WARNING -- the number of master processes was set to a value >= MPI_Comm_size, the value of \"-p\" has been reset to " << _sizeWorld / 2 << std::endl;
    }
    if(p == 1) {
        MPI_Comm_dup(comm, &_gatherComm);
        MPI_Comm_dup(comm, &_scatterComm);
        if(_rankWorld != 0)
            Solver<K>::_communicator = MPI_COMM_NULL;
        else
            Solver<K>::_communicator = MPI_COMM_SELF;
        Solver<K>::_ldistribution = new int[1]();
    }
    else {
        MPI_Group master, split;
        MPI_Group world;
        MPI_Comm_group(comm, &world);
        int* ps;
        unsigned int tmp;
        Solver<K>::_ldistribution = new int[p];
        if(T == 0) {
            if(_rankWorld < (p - 1) * (_sizeWorld / p))
                tmp = _sizeWorld / p;
            else
                tmp = _sizeWorld - (p - 1) * (_sizeWorld / p);
            ps = new int[tmp];
            unsigned int offset;
            if(tmp != _sizeWorld / p)
                offset = _sizeWorld - tmp;
            else
                offset = (_sizeWorld / p) * (_rankWorld / (_sizeWorld / p));
            std::iota(ps, ps + tmp, offset);
            for(unsigned short i = 0; i < p; ++i)
                Solver<K>::_ldistribution[i] = i * (_sizeWorld / p);
        }
        else if(T == 1) {
            if(_rankWorld == p - 1 || _rankWorld > p - 1 + (p - 1) * ((_sizeWorld - p) / p))
                tmp = _sizeWorld - (p - 1) * (_sizeWorld / p);
            else
                tmp = _sizeWorld / p;
            ps = new int[tmp];
            if(_rankWorld < p)
                ps[0] = _rankWorld;
            else {
                if(tmp == _sizeWorld / p)
                    ps[0] = (_rankWorld - p) / ((_sizeWorld - p) / p);
                else
                    ps[0] = p - 1;
            }
            unsigned int offset = ps[0] * (_sizeWorld / p - 1) + p - 1;
            std::iota(ps + 1, ps + tmp, offset + 1);
            std::iota(Solver<K>::_ldistribution, Solver<K>::_ldistribution + p, 0);
        }
        else if(T == 2) {
            // Here, it is assumed that all subdomains have the same number of coarse degrees of freedom as the rank 0 ! (only true when the distribution is uniform)
            float area = _sizeWorld *_sizeWorld / (2.0 * p);
            *Solver<K>::_ldistribution = 0;
            for(unsigned short i = 1; i < p; ++i)
                Solver<K>::_ldistribution[i] = static_cast<int>(_sizeWorld - std::sqrt(std::max(_sizeWorld * _sizeWorld - 2 * _sizeWorld * Solver<K>::_ldistribution[i - 1] - 2 * area + Solver<K>::_ldistribution[i - 1] * Solver<K>::_ldistribution[i - 1], 1.0f)) + 0.5);
            int* idx = std::upper_bound(Solver<K>::_ldistribution, Solver<K>::_ldistribution + p, _rankWorld);
            unsigned short i = idx - Solver<K>::_ldistribution;
            tmp = (i == p) ? _sizeWorld - Solver<K>::_ldistribution[i - 1] : Solver<K>::_ldistribution[i] - Solver<K>::_ldistribution[i - 1];
            ps = new int[tmp];
            for(unsigned int j = 0; j < tmp; ++j)
                ps[j] = Solver<K>::_ldistribution[i - 1] + j;
        }
        MPI_Group_incl(world, p, Solver<K>::_ldistribution, &master);
        MPI_Group_incl(world, tmp, ps, &split);
        delete [] ps;

        MPI_Comm_create(comm, master, &(Solver<K>::_communicator));
        MPI_Comm_create(comm, split, &_scatterComm);

        MPI_Group_free(&master);
        MPI_Group_free(&split);

        if(!exclude)
            MPI_Comm_dup(comm, &_gatherComm);
        else {
            MPI_Group global;
            MPI_Group_excl(world, p - 1, Solver<K>::_ldistribution + 1, &global);
            MPI_Comm_create(comm, global, &_gatherComm);
            MPI_Group_free(&global);
        }
        MPI_Group_free(&world);
    }
}

template<template<class> class Solver, char S, class K>
template<bool U, typename Solver<K>::Distribution D, bool excluded>
inline void CoarseOperator<Solver, S, K>::constructionCollective(const unsigned short* info, unsigned short p, const unsigned short* infoSplit) {
    if(!U) {
        if(excluded)
            _sizeWorld -= p;
        Solver<K>::_gatherCounts = new int[_sizeWorld];
        Solver<K>::_displs = new int[_sizeWorld];

        Solver<K>::_displs[0] = 0;
        Solver<K>::_gatherCounts[0] = info[0];
        for(unsigned int i = 1, j = 1; j < _sizeWorld; ++i)
            if(!excluded || info[i] != 0)
                Solver<K>::_gatherCounts[j++] = info[i];
        std::partial_sum(Solver<K>::_gatherCounts, Solver<K>::_gatherCounts + _sizeWorld - 1, Solver<K>::_displs + 1);
        if(excluded)
            _sizeWorld += p;
        if(D == DMatrix::DISTRIBUTED_SOL) {
            Solver<K>::_gatherSplitCounts = new int[_sizeSplit];
            Solver<K>::_displsSplit = new int[_sizeSplit];
            Solver<K>::_displsSplit[0] = 0;
            for(unsigned int i = 0; i < _sizeSplit; ++i)
                Solver<K>::_gatherSplitCounts[i] = infoSplit[i];
            std::partial_sum(Solver<K>::_gatherSplitCounts, Solver<K>::_gatherSplitCounts + _sizeSplit - 1, Solver<K>::_displsSplit + 1);
        }
    }
    else {
        Solver<K>::_gatherCounts = new int[1];
        *Solver<K>::_gatherCounts = _local;
    }
}

template<template<class> class Solver, char S, class K>
template<char T, bool U, bool excluded>
inline void CoarseOperator<Solver, S, K>::constructionMap(unsigned short p, const unsigned short* info) {
    if(T == 0) {
        if(!U) {
            int accumulate = 0;
            for(unsigned short i = 0; i < p - 1; ++i) {
                Solver<K>::_ldistribution[i] = std::accumulate(info + i * (_sizeWorld / p), info + (i + 1) * (_sizeWorld / p), 0);
                accumulate += Solver<K>::_ldistribution[i];
            }
            Solver<K>::_ldistribution[p - 1] = Solver<K>::_n - accumulate;
        }
        else {
            if(p == 1)
                *Solver<K>::_ldistribution = Solver<K>::_n;
            else {
                std::fill(Solver<K>::_ldistribution, Solver<K>::_ldistribution + p - 1, _local * (_sizeWorld / p - excluded));
                Solver<K>::_ldistribution[p - 1] = Solver<K>::_n - _local * (_sizeWorld / p - excluded) * (p - 1);
            }
        }
    }
    else if(T == 1) {
        Solver<K>::_idistribution = new int[Solver<K>::_n];
        unsigned int j;
        if(excluded)
            j = 0;
        else
            for(unsigned int i = 0; i < p * (_sizeWorld / p); ++i) {
                unsigned int offset;
                if(i % (_sizeWorld / p) == 0) {
                    j = i / (_sizeWorld / p);
                    offset = U ? (_sizeWorld / p) * _local * j : (std::accumulate(info, info + j, 0) + std::accumulate(info + p, info + p + j * (_sizeWorld / p - 1), 0));
                }
                else {
                    j = p - 1 + i - i / (_sizeWorld / p);
                    offset  = U ? _local * (1 + i  / (_sizeWorld / p)) : std::accumulate(info, info + 1 + i / (_sizeWorld / p), 0);
                    offset += U ? (j - p) * _local : std::accumulate(info + p, info + j, 0);
                }
                std::iota(Solver<K>::_idistribution + offset, Solver<K>::_idistribution + offset + (U ? _local : info[j]), U ? _local * j : std::accumulate(info, info + j, 0));
                if(i % (_sizeWorld / p) != 0)
                    j = offset + (U ? _local : info[j]);
            }
        std::iota(Solver<K>::_idistribution + j, Solver<K>::_idistribution + Solver<K>::_n, j);
        if(!U) {
            int accumulate = 0;
            for(unsigned short i = 0; i < p - 1; ++i) {
                Solver<K>::_ldistribution[i] = std::accumulate(info + p + i * (_sizeWorld / p - 1), info + p + (i + 1) * (_sizeWorld / p - 1), info[i]);
                accumulate += Solver<K>::_ldistribution[i];
            }
            Solver<K>::_ldistribution[p - 1] = Solver<K>::_n - accumulate;
        }
        else {
            std::fill(Solver<K>::_ldistribution, Solver<K>::_ldistribution + p - 1, _local * (_sizeWorld / p - excluded));
            Solver<K>::_ldistribution[p - 1] = Solver<K>::_n - _local * (_sizeWorld / p - excluded) * (p - 1);
        }
    }
    else if(T == 2) {
        if(!U) {
            unsigned int accumulate = 0;
            for(unsigned short i = 0; i < p - 1; ++i) {
                Solver<K>::_ldistribution[i] = std::accumulate(info + Solver<K>::_ldistribution[i], info + Solver<K>::_ldistribution[i + 1], 0);
                accumulate += Solver<K>::_ldistribution[i];
            }
            Solver<K>::_ldistribution[p - 1] = Solver<K>::_n - accumulate;
        }
        else {
            for(unsigned short i = 0; i < p - 1; ++i)
                Solver<K>::_ldistribution[i] = (Solver<K>::_ldistribution[i + 1] - Solver<K>::_ldistribution[i] - excluded) * _local;
            Solver<K>::_ldistribution[p - 1] = Solver<K>::_n - (Solver<K>::_ldistribution[p - 1] - (excluded ? p - 1 : 0)) * _local;
        }
    }
}

template<template<class> class Solver, char S, class K>
template<unsigned short U, unsigned short excluded, class Operator, class Container>
inline std::pair<MPI_Request, const K*>* CoarseOperator<Solver, S, K>::construction(Operator& v, const MPI_Comm& comm, Container& parm) {
    static_assert(Solver<K>::_numbering == 'F' || Solver<K>::_numbering == 'C', "Unknown numbering");
    static_assert(S == 'S' || S == 'G', "Unknown symmetry");
    static_assert(Operator::_pattern == 's' || Operator::_pattern == 'c', "Unknown pattern");
    if(std::is_same<Solver<K>, SuiteSparse<K>>::value)
        if(parm[P] != 1) {
            int rank;
            MPI_Comm_rank(comm, &rank);
            if(rank == 0)
                std::cout << "WARNING -- only one master process supported by the " << demangle(typeid(Solver<K>).name()) << " interface, forcing P to one" << std::endl;
            parm[P] = 1;
        }
    switch(parm[TOPOLOGY]) {
#ifndef HPDDM_CONTIGUOUS
        case  1: constructionCommunicator<1, (excluded > 0)>(comm, parm[P]); break;
#endif
        case  2: constructionCommunicator<2, (excluded > 0)>(comm, parm[P]); break;
        default: constructionCommunicator<0, (excluded > 0)>(comm, parm[P]); break;
    }
    if(excluded > 0 && Solver<K>::_communicator != MPI_COMM_NULL) {
        int result;
        MPI_Comm_compare(v._p.getCommunicator(), Solver<K>::_communicator, &result);
        if(result != MPI_CONGRUENT)
            std::cerr << "The communicators for the coarse operator don't match those of the domain decomposition" << std::endl;
    }
    if(U == 2 && parm[NU] == 0)
        _offset = true;
    Solver<K>::initialize(parm);
    switch(parm[TOPOLOGY]) {
#ifndef HPDDM_CONTIGUOUS
        case  1: return constructionMatrix<1, U, excluded>(v, comm, parm[P]);
#endif
        case  2: return constructionMatrix<2, U, excluded>(v, comm, parm[P]);
        default: return constructionMatrix<0, U, excluded>(v, comm, parm[P]);
    }
}

template<template<class> class Solver, char S, class K>
template<char T, unsigned short U, unsigned short excluded, class Operator>
inline std::pair<MPI_Request, const K*>* CoarseOperator<Solver, S, K>::constructionMatrix(Operator& v, const MPI_Comm& comm, unsigned short p) {
    unsigned short info[(U != 1 ? 3 : 1) + HPDDM_MAXCO];
    const std::vector<unsigned short>& sparsity = v.getPattern();
    info[0] = sparsity.size(); // number of intersections
    int rank;
    MPI_Comm_rank(v._p.getCommunicator(), &rank);
    const unsigned short first = (S == 'S' ? std::distance(sparsity.cbegin(), std::upper_bound(sparsity.cbegin(), sparsity.cend(), rank)) : 0);
    int rankSplit;
    MPI_Comm_size(_scatterComm, &_sizeSplit);
    MPI_Comm_rank(_scatterComm, &rankSplit);
    unsigned short* infoNeighbor;

    K*     sendMaster;
    unsigned int size;
    int* I;
    int* J;
    K*   C;

    if(U != 1) {
        infoNeighbor = new unsigned short[info[0]];
        info[1] = (excluded == 2 ? 0 : _local); // number of eigenvalues
        std::vector<MPI_Request> sendInfo;
        sendInfo.reserve(info[0]);
        MPI_Request rq;
        const unsigned short l = _local;
        if(excluded == 0) {
            if(T != 2)
                for(unsigned short i = 0; i < info[0]; ++i) {
                    if(!(T == 1 && sparsity[i] < p) &&
                       !(T == 0 && (sparsity[i] % (_sizeWorld / p) == 0) && sparsity[i] < p * (_sizeWorld / p))) {
                        MPI_Isend(const_cast<unsigned short*>(&l), 1, MPI_UNSIGNED_SHORT, sparsity[i], 1, v._p.getCommunicator(), &rq);
                        sendInfo.emplace_back(rq);
                    }
                }
            else
                for(unsigned short i = 0; i < info[0]; ++i) {
                    if(!std::binary_search(Solver<K>::_ldistribution, Solver<K>::_ldistribution + p, sparsity[i])) {
                        MPI_Isend(const_cast<unsigned short*>(&l), 1, MPI_UNSIGNED_SHORT, sparsity[i], 1, v._p.getCommunicator(), &rq);
                        sendInfo.emplace_back(rq);
                    }
                }
        }
        else if(excluded < 2) {
            for(unsigned short i = 0; i < info[0]; ++i) {
                MPI_Isend(const_cast<unsigned short*>(&l), 1, MPI_UNSIGNED_SHORT, sparsity[i], 1, v._p.getCommunicator(), &rq);
                sendInfo.emplace_back(rq);
            }
        }
        if(rankSplit != 0) {
            MPI_Request recvInfo[info[0]];
            for(unsigned short i = 0; i < info[0]; ++i)
                MPI_Irecv(infoNeighbor + i, 1, MPI_UNSIGNED_SHORT, sparsity[i], 1, v._p.getCommunicator(), recvInfo + i);
            unsigned int tmp = (S != 'S' ? _local : 0);
            for(unsigned short i = 0; i < info[0]; ++i) {
                int index;
                MPI_Waitany(info[0], recvInfo, &index, MPI_STATUS_IGNORE);
                if(!(S == 'S' && sparsity[index] < rank))
                    tmp += infoNeighbor[index];
            }
            if(S != 'S')
                size = _local * tmp;
            else {
                info[0] -= first;
                size = _local * tmp + _local * (_local + 1) / 2;
            }
            info[2] = size;
            if(_local) {
                sendMaster = new K[size];
                if(excluded == 0)
                    std::copy_n(sparsity.cbegin() + first, info[0], info + (U != 1 ? 3 : 1));
                else {
                    if(T == 0 || T == 2) {
                        for(unsigned short i = 0; i < info[0]; ++i) {
                            unsigned short j = 0;
                            info[(U != 1 ? 3 : 1) + i] = sparsity[i + first] + 1;
                            while(j < p - 1 && info[(U != 1 ? 3 : 1) + i] >= (T == 0 ? (_sizeWorld / p) * (j + 1) : Solver<K>::_ldistribution[j + 1])) {
                                ++info[(U != 1 ? 3 : 1) + i];
                                ++j;
                            }
                        }
                    }
                    else if(T == 1) {
                        for(unsigned short i = 0; i < info[0]; ++i)
                            info[(U != 1 ? 3 : 1) + i] = p + sparsity[i + first];
                    }
                }
            }
        }
        MPI_Waitall(sendInfo.size(), sendInfo.data(), MPI_STATUSES_IGNORE);
    }
    else {
        infoNeighbor = nullptr;
        if(rankSplit != 0) {
            if(S == 'S') {
                info[0] -= first;
                size = _local * _local * info[0] + _local * (_local + 1) / 2;
            }
            else
                size = _local * _local * (1 + info[0]);
            sendMaster = new K[size];
            std::copy_n(sparsity.cbegin() + first, info[0], info + (U != 1 ? 3 : 1));
        }
    }
    unsigned short (*infoSplit)[(U != 1 ? 3 : 1) + HPDDM_MAXCO];
    unsigned int*   offsetIdx;
    unsigned short* infoWorld;

    unsigned int offset;
#ifdef HPDDM_CSR_CO
    unsigned int nrow;
#ifdef HPDDM_LOC2GLOB
    int* loc2glob;
#endif
#endif

    if(rankSplit != 0)
        MPI_Gather(info, (U != 1 ? 3 : 1) + HPDDM_MAXCO, MPI_UNSIGNED_SHORT, NULL, (U != 1 ? 3 : 1) + HPDDM_MAXCO, MPI_UNSIGNED_SHORT, 0, _scatterComm);
    else {
        size = 0;
        infoSplit = new unsigned short[_sizeSplit][(U != 1 ? 3 : 1) + HPDDM_MAXCO];
        MPI_Gather(info, (U != 1 ? 3 : 1) + HPDDM_MAXCO, MPI_UNSIGNED_SHORT, reinterpret_cast<unsigned short*>(infoSplit), (U != 1 ? 3 : 1) + HPDDM_MAXCO, MPI_UNSIGNED_SHORT, 0, _scatterComm);
        if(S == 'S' && Operator::_pattern == 's')
            **infoSplit -= first;
        offsetIdx = new unsigned int[_sizeSplit - 1];
        if(U != 1) {
            infoWorld = new unsigned short[_sizeWorld];
            int recvcounts[p];
            int displs[p];
            displs[0] = 0;
            if(T == 2) {
                std::adjacent_difference(Solver<K>::_ldistribution + 1, Solver<K>::_ldistribution + p, recvcounts);
                recvcounts[p - 1] = _sizeWorld - Solver<K>::_ldistribution[p - 1];
            }
            else {
                std::fill(recvcounts, recvcounts + p - 1, _sizeWorld / p);
                recvcounts[p - 1] = _sizeWorld - (p - 1) * (_sizeWorld / p);
            }
            std::partial_sum(recvcounts, recvcounts + p - 1, displs + 1);
            for(unsigned int i = 0; i < _sizeSplit; ++i)
                infoWorld[displs[Solver<K>::_rank] + i] = infoSplit[i][1];
#ifdef HPDDM_CSR_CO
            nrow = std::accumulate(infoWorld + displs[Solver<K>::_rank], infoWorld + displs[Solver<K>::_rank] + _sizeSplit, 0);
            I = new int[nrow + 1];
            I[0] = (Solver<K>::_numbering == 'F');
#ifdef HPDDM_LOC2GLOB
#ifndef HPDDM_CONTIGUOUS
            loc2glob = new int[nrow];
#else
            loc2glob = new int[2];
#endif
#endif
#endif
            MPI_Allgatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, infoWorld, recvcounts, displs, MPI_UNSIGNED_SHORT, Solver<K>::_communicator);
            if(T == 1) {
                unsigned short perm[p - 1];
                unsigned int i = (p - 1) * (_sizeWorld / p);
                for(unsigned short k = p - 1, j = 1; k-- > 0; i -= _sizeWorld / p, ++j) {
                    perm[k] = infoWorld[i];
                    std::copy_backward(infoWorld + k * (_sizeWorld / p), infoWorld + (k + 1) * (_sizeWorld / p), infoWorld + (k + 1) * (_sizeWorld / p) + j);
                }
                std::copy(perm, perm + p - 1, infoWorld + 1);
            }
            offset = std::accumulate(infoWorld, infoWorld + _rankWorld, 0);
            Solver<K>::_n = std::accumulate(infoWorld + _rankWorld, infoWorld + _sizeWorld, offset);
            if(Solver<K>::_numbering == 'F')
                ++offset;
            unsigned short tmp = 0;
            for(unsigned short i = 0; i < info[0]; ++i) {
                infoNeighbor[i] = infoWorld[sparsity[i]];
                if(!(S == 'S' && i < first))
                    tmp += infoNeighbor[i];
            }
            for(unsigned short k = 1; k < _sizeSplit; ++k) {
                offsetIdx[k - 1] = size;
                size += infoSplit[k][2];
            }
            if(excluded < 2)
                size += _local * tmp + (S == 'S' ? _local * (_local + 1) / 2 : _local * _local);
            if(S == 'S')
                info[0] -= first;
        }
        else {
            Solver<K>::_n = (_sizeWorld - (excluded == 2 ? p : 0)) * _local;
            offset = (_rankWorld - (excluded == 2 ? rank : 0)) * _local + (Solver<K>::_numbering == 'F');
#ifdef HPDDM_CSR_CO
            nrow = (_sizeSplit - (excluded == 2)) * _local;
            I = new int[nrow + 1];
            I[0] = (Solver<K>::_numbering == 'F');
#ifdef HPDDM_LOC2GLOB
#ifndef HPDDM_CONTIGUOUS
            loc2glob = new int[nrow];
#else
            loc2glob = new int[2];
#endif
#endif
#endif
            if(S == 'S') {
                for(unsigned short i = 1; i < _sizeSplit; ++i) {
                    offsetIdx[i - 1] = size * _local * _local + (i - 1) * _local * (_local + 1) / 2;
                    size += infoSplit[i][0];
                }
                info[0] -= first;
                size = (size + info[0]) * _local * _local + _local * (_local + 1) / 2 * (_sizeSplit - (excluded == 2));
            } else {
                for(unsigned short i = 1; i < _sizeSplit; ++i) {
                    offsetIdx[i - 1] = (i - 1 + size) * _local * _local;
                    size += infoSplit[i][0];
                }
                size = (size + info[0] + _sizeSplit - (excluded == 2)) * _local * _local;
            }
        }
#ifndef HPDDM_CSR_CO
        I = new int[size];
#endif
        J = new int[size];
        C = new K[size];
    }
    const vectorNeighbor& M = v._p.getMap();

    std::vector<MPI_Request> rqSend;
    MPI_Request*             rqRecv;

    K** sendNeighbor;
    K** recvNeighbor = nullptr;
    int coefficients = (U == 1 ? _local * (info[0] + (S != 'S')) : std::accumulate(infoNeighbor + first, infoNeighbor + sparsity.size(), (S == 'S' ? 0 : _local)));
    if(Operator::_pattern == 's') {
#if HPDDM_ICOLLECTIVE
        rqRecv = new MPI_Request[info[0]];
#else
        rqRecv = new MPI_Request[rankSplit == 0 ? _sizeSplit - 1 + info[0] : info[0]];
#endif
        rqSend.reserve(S != 'S' ? info[0] : first);
        sendNeighbor = new K*[(S != 'S' ? info[0] : first)];
        for(unsigned short i = 0; i < (S != 'S' ? info[0] : first); ++i) {
            if(U == 1 || infoNeighbor[i])
                sendNeighbor[i] = new K[_local * M[i].second.size()];
            else
                sendNeighbor[i] = nullptr;
        }
        if(U == 1 || _local) {
            recvNeighbor = new K*[info[0]];
            for(unsigned short i = 0; i < info[0]; ++i) {
                recvNeighbor[i] = new K[(U == 1 ? _local : infoNeighbor[i + first]) * M[i + first].second.size()];
                MPI_Irecv(recvNeighbor[i], (U == 1 ? _local : infoNeighbor[i + first]) * M[i + first].second.size(), Wrapper<K>::mpi_type(), M[i + first].first, 2, v._p.getCommunicator(), rqRecv + i);
            }
        }
        else
            for(unsigned short i = 0; i < info[0]; ++i)
                rqRecv[i] = MPI_REQUEST_NULL;
    }
    else {
        rqSend.reserve(M.size());
#if HPDDM_ICOLLECTIVE
        rqRecv = new MPI_Request[M.size()];
#else
        rqRecv = new MPI_Request[rankSplit == 0 ? _sizeSplit - 1 + M.size() : M.size()];
#endif
        sendNeighbor = new K*[M.size()];
        if(U == 1 || _local)
            recvNeighbor = new K*[M.size()];
    }
    K* work = nullptr;
    if(Operator::_pattern == 's' && excluded < 2) {
        const K* const* const& EV = v._p.getVectors();
        const int n = v._p.getDof();
        v.initialize(n * (U == 1 || info[0] == 0 ? _local : std::max(static_cast<unsigned short>(_local), *std::max_element(infoNeighbor + first, infoNeighbor + sparsity.size()))), work, S != 'S' ? info[0] : first);
        v.template applyToNeighbor<S, U == 1>(sendNeighbor, work, rqSend, infoNeighbor);
        if(S != 'S') {
            unsigned short before = 0;
            for(unsigned short j = 0; sparsity[j] < rank && j < info[0]; ++j)
                before += (U == 1 ? _local : infoNeighbor[j]);
            K* const pt = (rankSplit != 0 ? sendMaster + before : C + before);
            Wrapper<K>::gemm(&(Wrapper<K>::transc), &transa, &_local, &_local, &n, &(Wrapper<K>::d__1), work, &n, *EV, &n, &(Wrapper<K>::d__0), pt, &coefficients);
            conjugate<K>(_local, _local, coefficients, pt);
            if(rankSplit == 0)
                for(unsigned short j = 0; j < _local; ++j) {
#ifndef HPDDM_CSR_CO
                    std::fill(I + before + j * coefficients, I + before + j * coefficients + _local, offset + j);
#endif
                    std::iota(J + before + j * coefficients, J + before + j * coefficients + _local, offset);
                }
        }
        else {
            if(rankSplit != 0)
                if(coefficients >= _local) {
                    Wrapper<K>::gemm(&(Wrapper<K>::transc), &transa, &_local, &_local, &n, &(Wrapper<K>::d__1), *EV, &n, work, &n, &(Wrapper<K>::d__0), sendMaster, &_local);
                    for(unsigned short j = _local; j-- > 0; )
                        std::copy_backward(sendMaster + j * (_local + 1), sendMaster + (j + 1) * _local, sendMaster - (j * (j + 1)) / 2 + j * coefficients + (j + 1) * _local);
                }
                else
                    for(unsigned short j = 0; j < _local; ++j) {
                        int local = _local - j;
                        Wrapper<K>::gemv(&(Wrapper<K>::transc), &n, &local, &(Wrapper<K>::d__1), EV[j], &n, work + n * j, &i__1, &(Wrapper<K>::d__0), sendMaster - (j * (j - 1)) / 2 + j * (coefficients + _local), &i__1);
                    }
            else {
                if(coefficients >= _local)
                    Wrapper<K>::gemm(&(Wrapper<K>::transc), &transa, &_local, &_local, &n, &(Wrapper<K>::d__1), *EV, &n, work, &n, &(Wrapper<K>::d__0), C, &_local);
                for(unsigned short j = _local; j-- > 0; ) {
#ifndef HPDDM_CSR_CO
                    std::fill(I + j * (coefficients + _local) - (j * (j - 1)) / 2, I + j * (coefficients + _local - 1) - (j * (j - 1)) / 2 + _local, offset + j);
#endif
                    std::iota(J + j * (coefficients + _local - 1) - (j * (j - 1)) / 2 + j, J + j * (coefficients + _local - 1) - (j * (j - 1)) / 2 + _local, offset + j);
                    if(coefficients >= _local)
                        std::copy_backward(C + j * (_local + 1), C + (j + 1) * _local, C - (j * (j + 1)) / 2 + j * coefficients + (j + 1) * _local);
                    else {
                        int local = _local - j;
                        Wrapper<K>::gemv(&(Wrapper<K>::transc), &n, &local, &(Wrapper<K>::d__1), EV[j], &n, work + n * j, &i__1, &(Wrapper<K>::d__0), C - (j * (j - 1)) / 2 + j * (coefficients + _local), &i__1);
                    }
                }
            }
        }
    }
    else if(Operator::_pattern != 's' && excluded < 2)
        v.template applyToNeighbor<S, U == 1>(sendNeighbor, work, rqSend, U == 1 ? nullptr : infoNeighbor, recvNeighbor, rqRecv);
    std::pair<MPI_Request, const K*>* ret = nullptr;
    if(rankSplit != 0) {
        if(U == 1 || _local) {
            if(Operator::_pattern == 's') {
                unsigned int* offsetArray = new unsigned int[info[0]];
                if(S != 'S')
                    offsetArray[0] = M[0].first > rank ? _local : 0;
                else if(info[0] > 0)
                    offsetArray[0] = _local;
                for(unsigned short k = 1; k < info[0]; ++k) {
                    offsetArray[k] = offsetArray[k - 1] + (U == 1 ? _local : infoNeighbor[k - 1 + first]);
                    if(S != 'S' && sparsity[k - 1] < rank && sparsity[k] > rank)
                        offsetArray[k] += _local;
                }
                for(unsigned short k = 0; k < info[0]; ++k) {
                    int index;
                    MPI_Waitany(info[0], rqRecv, &index, MPI_STATUS_IGNORE);
                    v.template assembleForMaster<S, U == 1>(sendMaster + offsetArray[index], recvNeighbor[index], coefficients + (S == 'S' ? _local - 1 : 0), index + first, work, infoNeighbor + first + index);
                }
                delete [] offsetArray;
            }
            else {
                for(unsigned short k = 0; k < M.size(); ++k) {
                    int index;
                    MPI_Waitany(M.size(), rqRecv, &index, MPI_STATUS_IGNORE);
                    v.template assembleForMaster<S, U == 1>(sendMaster, recvNeighbor[index], coefficients, index, work, infoNeighbor);
                }
            }
            if(excluded > 0) {
                ret = new std::pair<MPI_Request, const K*>(MPI_REQUEST_NULL, sendMaster);
#if HPDDM_ICOLLECTIVE
                MPI_Igatherv(sendMaster, size, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _scatterComm, &(ret->first));
#else
                MPI_Isend(sendMaster, size, Wrapper<K>::mpi_type(), 0, 3, _scatterComm, &(ret->first));
#endif
            }
            else {
#if HPDDM_ICOLLECTIVE
                MPI_Request rq;
                MPI_Igatherv(sendMaster, size, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _scatterComm, &rq);
                MPI_Wait(&rq, MPI_STATUS_IGNORE);
#else
                MPI_Send(sendMaster, size, Wrapper<K>::mpi_type(), 0, 3, _scatterComm);
#endif
                delete [] sendMaster;
            }
        }
        _sizeRHS = _local;
        if(U != 1)
            delete [] infoNeighbor;
        if(U == 0)
            Solver<K>::_displs = new int[1];
        MPI_Waitall(rqSend.size(), rqSend.data(), MPI_STATUSES_IGNORE);
        delete [] work;
    }
    else {
        unsigned short rankRelative = (T == 0 || T == 2) ? _rankWorld : p + _rankWorld * ((_sizeWorld / p) - 1) - 1;
        unsigned int* offsetPosition;
        unsigned int idx;
        if(excluded < 2) {
            idx = coefficients * _local + (S == 'S' ? (_local * (_local + 1)) / 2 : 0);
            for(unsigned short i = 0; i < _sizeSplit - 1; ++i)
                offsetIdx[i] += idx;
            if(Operator::_pattern == 's')
                idx = info[0];
            else
                idx = M.size();
        }
        else
            idx = 0;
#if HPDDM_ICOLLECTIVE
        int* counts = new int[2 * _sizeSplit];
        counts[0] = 0;
        counts[_sizeSplit] = (U == 1 ? (S == 'S' ? _local * infoSplit[0][0] * _local + _local * (_local + 1) / 2 : (_local * infoSplit[0][0] + _local) * _local) : infoSplit[0][2]);
        for(unsigned short k = 1; k < _sizeSplit; ++k) {
            counts[k] = offsetIdx[k - 1];
            counts[_sizeSplit + k] = (U == 1 ? (counts[_sizeSplit + k - 1] + (S == 'S' ? (_local * infoSplit[k][0] * _local + _local * (_local + 1) / 2) : ((_local * infoSplit[k][0] + _local) * _local))) : infoSplit[k][2]);
        }
        MPI_Request rq;
        MPI_Igatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, C, counts + _sizeSplit, counts, Wrapper<K>::mpi_type(), 0, _scatterComm, &rq);
#endif
        if(U != 1) {
#if !HPDDM_ICOLLECTIVE
            for(unsigned short k = 1; k < _sizeSplit; ++k) {
                if(infoSplit[k][2])
                    MPI_Irecv(C + offsetIdx[k - 1], infoSplit[k][2], Wrapper<K>::mpi_type(), k, 3, _scatterComm, rqRecv + idx + k - 1);
                else
                    rqRecv[idx + k - 1] = MPI_REQUEST_NULL;
            }
#endif
            offsetPosition = new unsigned int[_sizeSplit];
            offsetPosition[0] = std::accumulate(infoWorld, infoWorld + rankRelative, static_cast<unsigned int>(Solver<K>::_numbering == 'F'));
            if(T == 0 || T == 2)
                for(unsigned int k = 1; k < _sizeSplit; ++k)
                    offsetPosition[k] = offsetPosition[k - 1] + infoSplit[k - 1][1];
            else if(T == 1)
                for(unsigned int k = 1; k < _sizeSplit; ++k)
                    offsetPosition[k] = offsetPosition[k - 1] + infoWorld[rankRelative + k - 1];
        }
#if !HPDDM_ICOLLECTIVE
        else {
            for(unsigned short k = 1; k < _sizeSplit; ++k)
                MPI_Irecv(C + offsetIdx[k - 1], S == 'S' ? _local * infoSplit[k][0] * _local + _local * (_local + 1) / 2 : (_local * infoSplit[k][0] + _local) * _local, Wrapper<K>::mpi_type(), k, 3, _scatterComm, rqRecv + idx + k - 1);
        }
#endif
#pragma omp parallel for shared(I, J, infoWorld, infoSplit, rankRelative, offsetIdx, offsetPosition) schedule(dynamic, 64)
        for(unsigned int k = 1; k < _sizeSplit; ++k) {
            if(U == 1 || infoSplit[k][2]) {
                unsigned int tmp = U == 1 ? (rankRelative + k - (excluded == 2 ? (T == 1 ? p : 1 + rank) : 0)) * _local + (Solver<K>::_numbering == 'F') : offsetPosition[k];
                unsigned int idxSlave = offsetIdx[k - 1];
                unsigned int offsetSlave;
                if(U != 1)
                    offsetSlave = std::accumulate(infoWorld, infoWorld + infoSplit[k][U != 1 ? 3 : 1], static_cast<unsigned int>(Solver<K>::_numbering == 'F'));
                unsigned short i = 0;
                if(S != 'S')
                    for( ; infoSplit[k][(U != 1 ? 3 : 1) + i] < rankRelative + k - (U == 1 && excluded == 2 ? (T == 1 ? p : 1 + rank) : 0) && i < infoSplit[k][0]; ++i) {
                        if(U != 1) {
                            if(i > 0)
                                offsetSlave = std::accumulate(infoWorld + infoSplit[k][U != 1 ? 2 + i : i], infoWorld + infoSplit[k][(U != 1 ? 3 : 1) + i], offsetSlave);
                        }
                        else
                            offsetSlave = infoSplit[k][(U != 1 ? 3 : 1) + i] * _local + (Solver<K>::_numbering == 'F');
                        for(unsigned int j = offsetSlave; j < offsetSlave + (U == 1 ? _local : infoWorld[infoSplit[k][(U != 1 ? 3 : 1) + i]]); ++j)
                            J[idxSlave++] = j;
                    }
                for(unsigned int j = tmp; j < tmp + (U == 1 ? _local : infoSplit[k][1]); ++j)
                    J[idxSlave++] = j;
                for( ; i < infoSplit[k][0]; ++i) {
                    if(U != 1) {
                        if(i > 0)
                            offsetSlave = std::accumulate(infoWorld + infoSplit[k][U != 1 ? 2 + i : i], infoWorld + infoSplit[k][(U != 1 ? 3 : 1) + i], offsetSlave);
                    }
                    else
                        offsetSlave = infoSplit[k][(U != 1 ? 3 : 1) + i] * _local + (Solver<K>::_numbering == 'F');
                    for(unsigned int j = offsetSlave; j < offsetSlave + (U == 1 ? _local : infoWorld[infoSplit[k][(U != 1 ? 3 : 1) + i]]); ++j)
                        J[idxSlave++] = j;
                }
                int coefficientsSlave = idxSlave - offsetIdx[k - 1];
#ifndef HPDDM_CSR_CO
                std::fill(I + offsetIdx[k - 1], I + offsetIdx[k - 1] + coefficientsSlave, tmp);
#else
                offsetSlave = U == 1 ? (k - (excluded == 2)) * _local : offsetPosition[k] - offsetPosition[1] + (excluded == 2 ? 0 : _local);
                I[offsetSlave + 1] = coefficientsSlave;
#if defined(HPDDM_LOC2GLOB) && !defined(HPDDM_CONTIGUOUS)
                loc2glob[offsetSlave] = tmp;
#endif
#endif
                for(i = 1; i < (U == 1 ? _local : infoSplit[k][1]); ++i, idxSlave += coefficientsSlave) {
                    if(S == 'S')
                        --coefficientsSlave;
#ifndef HPDDM_CSR_CO
                    std::fill(I + idxSlave, I + idxSlave + coefficientsSlave, tmp + i);
#else
                    I[offsetSlave + 1 + i] = coefficientsSlave;
#if defined(HPDDM_LOC2GLOB) && !defined(HPDDM_CONTIGUOUS)
                    loc2glob[offsetSlave + i] = tmp + i;
#endif
#endif
                    std::copy(J + idxSlave - coefficientsSlave, J + idxSlave, J + idxSlave);
                }
#if defined(HPDDM_LOC2GLOB) && defined(HPDDM_CONTIGUOUS)
                if(excluded == 2 && k == 1)
                    loc2glob[0] = tmp;
                if(k == _sizeSplit - 1)
                    loc2glob[1] = tmp + (U == 1 ? _local : infoSplit[k][1]) - 1;
#endif
            }
        }
        delete [] offsetIdx;
        if(excluded < 2) {
            unsigned int (*offsetArray)[(Operator::_pattern == 's') + (U != 1)] = new unsigned int[info[0]][(Operator::_pattern == 's') + (U != 1)];
#ifdef HPDDM_CSR_CO
            for(unsigned short k = 0; k < _local; ++k) {
                I[k + 1] = coefficients + (S == 'S' ? _local - k : 0);
#if defined(HPDDM_LOC2GLOB) && !defined(HPDDM_CONTIGUOUS)
                loc2glob[k] = offset + k;
#endif
            }
#if defined(HPDDM_LOC2GLOB) && defined(HPDDM_CONTIGUOUS)
            loc2glob[0] = offset;
            if(_sizeSplit == 1)
                loc2glob[1] = offset + _local - 1;
#endif
#endif
            if(Operator::_pattern == 's') {
                if(S != 'S') {
                    offsetArray[0][0] = sparsity[0] > _rankWorld ? _local : 0;
                    if(U != 1)
                        offsetArray[0][1] = std::accumulate(infoWorld, infoWorld + sparsity[0], static_cast<unsigned int>(Solver<K>::_numbering == 'F'));
                }
                else {
                    if(info[0] > 0) {
                        offsetArray[0][0] = _local;
                        if(U != 1)
                            offsetArray[0][1] = std::accumulate(infoWorld, infoWorld + sparsity[first], static_cast<unsigned int>(Solver<K>::_numbering == 'F'));
                    }
                }
                for(unsigned short k = 1; k < info[0]; ++k) {
                    if(U != 1) {
                        offsetArray[k][1] = std::accumulate(infoWorld + sparsity[first + k - 1], infoWorld + sparsity[first + k], offsetArray[k - 1][1]);
                        offsetArray[k][0] = offsetArray[k - 1][0] + infoNeighbor[k - 1 + first];
                    }
                    else
                        offsetArray[k][0] = offsetArray[k - 1][0] + _local;
                    if((S != 'S') && sparsity[k - 1] < _rankWorld && sparsity[k] > _rankWorld)
                        offsetArray[k][0] += _local;
                }
            }
            else {
                if(U != 1) {
                    if(S != 'S')
                        offsetArray[0][0] = std::accumulate(infoWorld, infoWorld + sparsity[0], static_cast<unsigned int>(Solver<K>::_numbering == 'F'));
                    else if(info[0] > 0)
                        offsetArray[0][0] = std::accumulate(infoWorld, infoWorld + sparsity[first], static_cast<unsigned int>(Solver<K>::_numbering == 'F'));
                    for(unsigned short k = 1; k < info[0]; ++k)
                        offsetArray[k][0] = std::accumulate(infoWorld + sparsity[first + k - 1], infoWorld + sparsity[first + k], offsetArray[k - 1][0]);
                }
                info[0] = M.size();
            }
            if(U == 1 || _local)
                for(unsigned int k = 0; k < info[0]; ++k) {
                    int index;
                    MPI_Waitany(info[0], rqRecv, &index, MPI_STATUS_IGNORE);
                    if(Operator::_pattern == 's')
                        v.template applyFromNeighborMaster<S, Solver<K>::_numbering, U == 1>(recvNeighbor[index], index + first, I + offsetArray[index][0], J + offsetArray[index][0], C + offsetArray[index][0], coefficients + (S == 'S') * (_local - 1), offset, U == 1 ? nullptr : (offsetArray[index] + 1), work, U == 1 ? nullptr : infoNeighbor + first + index);
                    else
                        v.template applyFromNeighborMaster<S, Solver<K>::_numbering, U == 1>(recvNeighbor[index], index, I, J, C, coefficients, offset, U == 1 ? nullptr : *offsetArray, work, U == 1 ? nullptr : infoNeighbor);
                }
            delete [] offsetArray;
        }
#if !HPDDM_ICOLLECTIVE
        MPI_Waitall(_sizeSplit - 1, rqRecv + idx, MPI_STATUSES_IGNORE);
#else
        MPI_Wait(&rq, MPI_STATUS_IGNORE);
#endif
#if HPDDM_ICOLLECTIVE
        delete [] counts;
#endif
        if(U != 1) {
            delete [] infoNeighbor;
            delete [] offsetPosition;
        }
        delete [] work;
#if HPDDM_OUTPUT_CO
        std::string fileName = "E_distributed_";
        if(excluded == 2)
            fileName += "excluded_";
        std::ofstream txtE{ fileName + S + "_" + Solver<K>::_numbering + "_" + std::to_string(T) + "_" + std::to_string(Solver<K>::_rank) + ".txt" };
#ifndef HPDDM_CSR_CO
        for(unsigned int i = 0; i < size; ++i)
            txtE << "(" << std::setw(4) << I[i] << ", " << std::setw(4) << J[i] << ") = " << std::scientific << C[i] << std::endl;
#else
        unsigned int acc = 0;
        for(unsigned int i = 0; i < nrow; ++i) {
            acc += I[i];
#ifndef HPDDM_LOC2GLOB
            for(unsigned int j = 0; j < I[i + 1]; ++j)
                txtE << "(" << std::setw(4) << i << ", " << std::setw(4) << J[acc + j - (Solver<K>::_numbering == 'F')] << ") = " << std::scientific << C[acc + j - (Solver<K>::_numbering == 'F')] << " (" << I[i] << " -- " << I[i + 1] << ")" << std::endl;
#else
#ifndef HPDDM_CONTIGUOUS
            for(unsigned int j = 0; j < I[i + 1]; ++j)
                txtE << "(" << std::setw(4) << loc2glob[i] << ", " << std::setw(4) << J[acc + j - (Solver<K>::_numbering == 'F')] << ") = " << std::scientific << C[acc + j - (Solver<K>::_numbering == 'F')] << " (" << I[i] << " -- " << I[i + 1] << ")" << std::endl;
#else
            for(unsigned int j = 0; j < I[i + 1]; ++j)
                txtE << "(" << std::setw(4) << loc2glob[0] + i << ", " << std::setw(4) << J[acc + j - (Solver<K>::_numbering == 'F')] << ") = " << std::scientific << C[acc + j - (Solver<K>::_numbering == 'F')] << " (" << I[i] << " -- " << I[i + 1] << ")" << std::endl;
#endif
#endif
        }
#endif
        txtE.close();
        MPI_Barrier(Solver<K>::_communicator);
#endif
#ifdef HPDDM_CSR_CO
        for(unsigned int i = 0; i < nrow; ++i)
            I[i + 1] += I[i];
#ifndef HPDDM_LOC2GLOB
        Solver<K>::template numfact<S>(nrow, I, J, C);
#else
        Solver<K>::template numfact<S>(nrow, I, loc2glob, J, C);
#endif
#else
        Solver<K>::template numfact<S>(size, I, J, C);
#endif

#ifdef DMKL_PARDISO
        if(S == 'S' || p != 1)
            delete [] C;
#else
        delete [] C;
#endif
    }
    delete [] rqRecv;

    if(Operator::_pattern != 's')
        info[0] = M.size();
    for(unsigned short i = 0; i < (S != 'S' || Operator::_pattern != 's' ? info[0] : first); ++i)
        delete [] sendNeighbor[i];
    if(U == 1 || _local)
        for(unsigned short i = 0; i < info[0]; ++i)
            delete [] recvNeighbor[i];
    delete [] sendNeighbor;
    delete [] recvNeighbor;
    if(U != 2) {
        switch(Solver<K>::_distribution) {
            case DMatrix::NON_DISTRIBUTED:
                _scatterComm = _gatherComm;
                break;
            case DMatrix::DISTRIBUTED_SOL:
                break;
            case DMatrix::DISTRIBUTED_SOL_AND_RHS:
                _gatherComm = _scatterComm;
                break;
        }
    }
    else {
        unsigned short* pt;
        unsigned short size;
        switch(Solver<K>::_distribution) {
            case DMatrix::NON_DISTRIBUTED:
                if(rankSplit != 0)
                    infoWorld = new unsigned short[_sizeWorld];
                pt = infoWorld;
                size = _sizeWorld;
                break;
            case DMatrix::DISTRIBUTED_SOL:
                size = _sizeWorld + _sizeSplit;
                pt = new unsigned short[size];
                if(rankSplit == 0) {
                    std::copy(infoWorld, infoWorld + _sizeWorld, pt);
                    for(unsigned int i = 0; i < _sizeSplit; ++i)
                        pt[_sizeWorld + i] = infoSplit[i][1];
                }
                break;
            case DMatrix::DISTRIBUTED_SOL_AND_RHS:
                unsigned short* infoMaster;
                if(rankSplit == 0) {
                    infoMaster = infoSplit[0];
                    for(unsigned int i = 0; i < _sizeSplit; ++i)
                        infoMaster[i] = infoSplit[i][1];
                }
                else
                    infoMaster = new unsigned short[_sizeSplit];
                pt = infoMaster;
                size = _sizeSplit;
                break;
        }
        MPI_Bcast(pt, size, MPI_UNSIGNED_SHORT, 0, _scatterComm);
        if(Solver<K>::_distribution == DMatrix::NON_DISTRIBUTED || Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL_AND_RHS) {
            if(Solver<K>::_distribution == DMatrix::NON_DISTRIBUTED)
                constructionCommunicatorCollective<(excluded > 0)>(pt, size, _gatherComm, &_scatterComm);
            else
                constructionCommunicatorCollective<false>(pt, size, _scatterComm);
            _gatherComm = _scatterComm;
        }
        else if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL) {
            constructionCommunicatorCollective<(excluded > 0)>(pt, _sizeWorld, _gatherComm);
            constructionCommunicatorCollective<false>(pt + _sizeWorld, _sizeSplit, _scatterComm);
        }
        if(rankSplit != 0 || Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL)
            delete [] pt;
    }
    if(rankSplit == 0) {
        if(Solver<K>::_distribution == DMatrix::NON_DISTRIBUTED) {
            if(_rankWorld == 0) {
                _sizeRHS = Solver<K>::_n;
                if(U == 1)
                    constructionCollective<true, DMatrix::NON_DISTRIBUTED, excluded == 2>();
                else if(U == 2) {
                    Solver<K>::_gatherCounts = new int[1];
                    if(_local == 0) {
                        _local = *Solver<K>::_gatherCounts = *std::find_if(infoWorld, infoWorld + _sizeWorld, [](const unsigned short& nu) { return nu != 0; });
                        _sizeRHS += _local;
                    }
                    else
                        *Solver<K>::_gatherCounts = _local;
                }
                else
                    constructionCollective<false, DMatrix::NON_DISTRIBUTED, excluded == 2>(infoWorld, p - 1);
            }
            else {
                if(U == 0)
                    Solver<K>::_displs = new int[1];
                _sizeRHS = _local;
            }
        }
        else {
            constructionMap<T, U == 1, excluded == 2>(p, U == 1 ? nullptr : infoWorld);
            if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL) {
                if(_rankWorld == 0)
                    _sizeRHS = Solver<K>::_n;
                else
                    _sizeRHS = Solver<K>::_ldistribution[Solver<K>::_rank];
            }
            else
                _sizeRHS = Solver<K>::_ldistribution[Solver<K>::_rank];
            if(U == 1)
                constructionCollective<true, DMatrix::DISTRIBUTED_SOL, excluded == 2>();
            else if(U == 2) {
                Solver<K>::_gatherCounts = new int[1];
                if(_local == 0) {
                    _local = *Solver<K>::_gatherCounts = *std::find_if(infoWorld, infoWorld + _sizeWorld, [](const unsigned short& nu) { return nu != 0; });
                    _sizeRHS += _local;
                }
                else
                    *Solver<K>::_gatherCounts = _local;
            }
            else {
                unsigned short* infoMaster = infoSplit[0];
                for(unsigned int i = 0; i < _sizeSplit; ++i)
                    infoMaster[i] = infoSplit[i][1];
                constructionCollective<false, DMatrix::DISTRIBUTED_SOL, excluded == 2>(infoWorld, p - 1, infoMaster);
            }
        }
        if(U != 1)
            delete [] infoWorld;
        delete [] infoSplit;
        if(excluded == 2) {
            if(Solver<K>::_distribution == DMatrix::NON_DISTRIBUTED && _rankWorld == 0)
                _sizeRHS += _local;
            else if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL || Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL_AND_RHS)
                _sizeRHS += _local;
        }
    }
    return ret;
}

template<template<class> class Solver, char S, class K>
template<bool excluded>
inline void CoarseOperator<Solver, S, K>::callSolver(K* const rhs, const int& fuse) {
    if(_scatterComm != MPI_COMM_NULL) {
        if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL) {
            if(Solver<K>::_displs) {
                if(_rankWorld == 0)                   MPI_Gatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, Solver<K>::_gatherCounts, Solver<K>::_displs, Wrapper<K>::mpi_type(), 0, _gatherComm);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Gatherv(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL>(rhs);
                    MPI_Scatterv(rhs, Solver<K>::_gatherSplitCounts, Solver<K>::_displsSplit, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm);
                }
                else
                    MPI_Scatterv(NULL, 0, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm);
            }
            else {
                if(_rankWorld == 0)                   MPI_Gather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Gather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL>(rhs + (_offset || excluded ? *Solver<K>::_gatherCounts : 0));
                    MPI_Scatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm);
                }
                else
                    MPI_Scatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm);
            }
        }
        else if(Solver<K>::_distribution == DMatrix::NON_DISTRIBUTED) {
            if(Solver<K>::_displs) {
                if(_rankWorld == 0)                   MPI_Gatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, Solver<K>::_gatherCounts, Solver<K>::_displs, Wrapper<K>::mpi_type(), 0, _gatherComm);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Gatherv(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                if(Solver<K>::_communicator != MPI_COMM_NULL)
                    Solver<K>::template solve<DMatrix::NON_DISTRIBUTED>(rhs);
                if(_rankWorld == 0)                   MPI_Scatterv(rhs, Solver<K>::_gatherCounts, Solver<K>::_displs, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Scatterv(NULL, 0, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _gatherComm);
            }
            else {
                if(_rankWorld == 0)                   MPI_Gather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm);
                else                                  MPI_Gather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                if(Solver<K>::_communicator != MPI_COMM_NULL)
                    Solver<K>::template solve<DMatrix::NON_DISTRIBUTED>(rhs + (_offset || excluded ? _local : 0));
                if(_rankWorld == 0)                   MPI_Scatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm);
                else                                  MPI_Scatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm);
            }
        }
        else if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL_AND_RHS) {
            if(Solver<K>::_displs) {
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    MPI_Gatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, Solver<K>::_gatherSplitCounts, Solver<K>::_displsSplit, Wrapper<K>::mpi_type(), 0, _gatherComm);
                    Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs);
                    MPI_Scatterv(rhs, Solver<K>::_gatherSplitCounts, Solver<K>::_displsSplit, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm);
                }
                else {
                    MPI_Gatherv(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                    MPI_Scatterv(NULL, 0, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm);
                }
            } else {
#if defined(DPASTIX) || defined(DMKL_PARDISO)
                if(fuse > 0) {
                    _local += fuse;
                    if(Solver<K>::_communicator != MPI_COMM_NULL) {
                        *Solver<K>::_gatherCounts += fuse;
                        MPI_Gather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm);
                        unsigned int end    = _sizeSplit * *Solver<K>::_gatherCounts - fuse;
                        K* pt = rhs + *Solver<K>::_gatherCounts - fuse;
                        for(unsigned int i = 1; i < _sizeSplit; ++i)
                            Wrapper<K>::axpy(&fuse, &(Wrapper<K>::d__1), pt + (i - 1) * *Solver<K>::_gatherCounts, &i__1, rhs + end, &i__1);
                        Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs + (_offset || excluded ? *Solver<K>::_gatherCounts : 0), fuse);
                        MPI_Allreduce(MPI_IN_PLACE, rhs + end, fuse, Wrapper<K>::mpi_type(), MPI_SUM, Solver<K>::_communicator);
                        for(unsigned int i = _sizeSplit - 1; i > 0; --i)
                            std::copy_n(rhs + end, fuse, pt + (i - 1) * *Solver<K>::_gatherCounts);
                        MPI_Scatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm);
                        *Solver<K>::_gatherCounts -= fuse;
                    }
                    else {
                        MPI_Gather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                        MPI_Scatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm);
                    }
                    _local -= fuse;
                }
                else {
#endif // DPASTIX || DMKL_PARDISO
                    if(Solver<K>::_communicator != MPI_COMM_NULL) {
                        MPI_Gather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm);
                        Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs + (_offset || excluded ? *Solver<K>::_gatherCounts : 0));
                        MPI_Scatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm);
                    }
                    else {
                        MPI_Gather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm);
                        MPI_Scatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm);
                    }
#if defined(DPASTIX) || defined(DMKL_PARDISO)
                }
#endif
            }
        }
    }
    else if(Solver<K>::_communicator != MPI_COMM_NULL) {
        switch(Solver<K>::_distribution) {
            case DMatrix::NON_DISTRIBUTED:         Solver<K>::template solve<DMatrix::NON_DISTRIBUTED>(rhs); break;
            case DMatrix::DISTRIBUTED_SOL:         Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL>(rhs); break;
            case DMatrix::DISTRIBUTED_SOL_AND_RHS: Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs); break;
        }
    }
}

#if HPDDM_ICOLLECTIVE
template<template<class> class Solver, char S, class K>
template<bool excluded>
inline void CoarseOperator<Solver, S, K>::IcallSolver(K* const rhs, MPI_Request* rq, const int& fuse) {
    if(_scatterComm != MPI_COMM_NULL) {
        if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL) {
            if(Solver<K>::_displs) {
                if(_rankWorld == 0)                   MPI_Igatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, Solver<K>::_gatherCounts, Solver<K>::_displs, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Igatherv(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    MPI_Wait(rq, MPI_STATUS_IGNORE);
                    Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL>(rhs);
                    MPI_Iscatterv(rhs, Solver<K>::_gatherSplitCounts, Solver<K>::_displsSplit, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm, rq + 1);
                }
                else
                    MPI_Iscatterv(NULL, 0, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm, rq + 1);
            }
            else {
                if(_rankWorld == 0)                   MPI_Igather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Igather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    MPI_Wait(rq, MPI_STATUS_IGNORE);
                    Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL>(rhs + (_offset || excluded ? *Solver<K>::_gatherCounts : 0));
                    MPI_Iscatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm, rq + 1);
                }
                else
                    MPI_Iscatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm, rq + 1);
            }
        }
        else if(Solver<K>::_distribution == DMatrix::NON_DISTRIBUTED) {
            if(Solver<K>::_displs) {
                if(_rankWorld == 0)                   MPI_Igatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, Solver<K>::_gatherCounts, Solver<K>::_displs, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Igatherv(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    MPI_Wait(rq, MPI_STATUS_IGNORE);
                    Solver<K>::template solve<DMatrix::NON_DISTRIBUTED>(rhs);
                }
                if(_rankWorld == 0)                   MPI_Iscatterv(rhs, Solver<K>::_gatherCounts, Solver<K>::_displs, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm, rq + 1);
                else if(_gatherComm != MPI_COMM_NULL) MPI_Iscatterv(NULL, 0, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm, rq + 1);
            }
            else {
                if(_rankWorld == 0)                   MPI_Igather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                else                                  MPI_Igather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    MPI_Wait(rq, MPI_STATUS_IGNORE);
                    Solver<K>::template solve<DMatrix::NON_DISTRIBUTED>(rhs + (_offset || excluded ? _local : 0));
                }
                if(_rankWorld == 0)                   MPI_Iscatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq + 1);
                else                                  MPI_Iscatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _gatherComm, rq + 1);
            }
        }
        else if(Solver<K>::_distribution == DMatrix::DISTRIBUTED_SOL_AND_RHS) {
            if(Solver<K>::_displs) {
                if(Solver<K>::_communicator != MPI_COMM_NULL) {
                    MPI_Igatherv(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, Solver<K>::_gatherSplitCounts, Solver<K>::_displsSplit, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                    MPI_Wait(rq, MPI_STATUS_IGNORE);
                    Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs);
                    MPI_Iscatterv(rhs, Solver<K>::_gatherSplitCounts, Solver<K>::_displsSplit, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm, rq + 1);
                }
                else {
                    MPI_Igatherv(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                    MPI_Iscatterv(NULL, 0, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm, rq + 1);
                }
            } else {
#if defined(DPASTIX) || defined(DMKL_PARDISO)
                if(fuse > 0) {
                    _local += fuse;
                    if(Solver<K>::_communicator != MPI_COMM_NULL) {
                        *Solver<K>::_gatherCounts += fuse;
                        MPI_Igather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                        MPI_Wait(rq, MPI_STATUS_IGNORE);
                        unsigned int end    = _sizeSplit * *Solver<K>::_gatherCounts - fuse;
                        K* pt = rhs + *Solver<K>::_gatherCounts - fuse;
                        for(unsigned int i = 1; i < _sizeSplit; ++i)
                            Wrapper<K>::axpy(&fuse, &(Wrapper<K>::d__1), pt + (i - 1) * *Solver<K>::_gatherCounts, &i__1, rhs + end, &i__1);
                        Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs + (_offset || excluded ? *Solver<K>::_gatherCounts : 0), fuse);
                        MPI_Allreduce(MPI_IN_PLACE, rhs + end, fuse, Wrapper<K>::mpi_type(), MPI_SUM, Solver<K>::_communicator);
                        for(unsigned int i = _sizeSplit - 1; i > 0; --i)
                            std::copy_n(rhs + end, fuse, pt + (i - 1) * *Solver<K>::_gatherCounts);
                        MPI_Iscatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm, rq + 1);
                        *Solver<K>::_gatherCounts -= fuse;
                    }
                    else {
                        MPI_Igather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                        MPI_Iscatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm, rq + 1);
                    }
                    _local -= fuse;
                }
                else {
#endif // DPASTIX || DMKL_PARDISO
                    if(Solver<K>::_communicator != MPI_COMM_NULL) {
                        MPI_Igather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), 0, _gatherComm, rq);
                        MPI_Wait(rq, MPI_STATUS_IGNORE);
                        Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs + (_offset || excluded ? *Solver<K>::_gatherCounts : 0));
                        MPI_Iscatter(rhs, *Solver<K>::_gatherCounts, Wrapper<K>::mpi_type(), MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, 0, _scatterComm, rq + 1);
                    }
                    else {
                        MPI_Igather(rhs, _local, Wrapper<K>::mpi_type(), NULL, 0, MPI_DATATYPE_NULL, 0, _gatherComm, rq);
                        MPI_Iscatter(NULL, 0, MPI_DATATYPE_NULL, rhs, _local, Wrapper<K>::mpi_type(), 0, _scatterComm, rq + 1);
                    }
#if defined(DPASTIX) || defined(DMKL_PARDISO)
                }
#endif
            }
        }
    }
    else if(Solver<K>::_communicator != MPI_COMM_NULL) {
        switch(Solver<K>::_distribution) {
            case DMatrix::NON_DISTRIBUTED:         Solver<K>::template solve<DMatrix::NON_DISTRIBUTED>(rhs); break;
            case DMatrix::DISTRIBUTED_SOL:         Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL>(rhs); break;
            case DMatrix::DISTRIBUTED_SOL_AND_RHS: Solver<K>::template solve<DMatrix::DISTRIBUTED_SOL_AND_RHS>(rhs); break;
        }
    }
}
#endif // HPDDM_ICOLLECTIVE
} // HPDDM
#endif // _COARSE_OPERATOR_IMPL_
