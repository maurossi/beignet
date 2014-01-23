/*
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file gen_reg_allocation.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */
#include "ir/profile.hpp"
#include "ir/function.hpp"
#include "backend/gen_insn_selection.hpp"
#include "backend/gen_reg_allocation.hpp"
#include "backend/gen_register.hpp"
#include "backend/program.hpp"
#include "sys/exception.hpp"
#include <algorithm>
#include <climits>
#include <iostream>
#include <iomanip>

#define RESERVED_REG_NUM_FOR_SPILL 6

namespace gbe
{
  /////////////////////////////////////////////////////////////////////////////
  // Register allocator internal implementation
  /////////////////////////////////////////////////////////////////////////////

  /*! Provides the location of a register in a vector */
  typedef std::pair<SelectionVector*, uint32_t> VectorLocation;

  /*! Implements the register allocation */
  class GenRegAllocator::Opaque
  {
  public:
    /*! Initialize the register allocator */
    Opaque(GenContext &ctx);
    /*! Release all taken resources */
    ~Opaque(void);
    /*! Perform the register allocation. Return true if success */
    bool allocate(Selection &selection);
    /*! Return the Gen register from the selection register */
    GenRegister genReg(const GenRegister &reg);
    /*! Output the register allocation */
    void outputAllocation(void);
  private:
    /*! Expire one GRF interval. Return true if one was successfully expired */
    bool expireGRF(const GenRegInterval &limit);
    /*! Expire a flag register. Return true if one was successfully expired */
    bool expireFlag(const GenRegInterval &limit);
    /*! Allocate the virtual boolean (== flags) registers */
    void allocateFlags(Selection &selection);
    /*! Allocate the GRF registers */
    bool allocateGRFs(Selection &selection);
    /*! Create gen registers for all preallocated curbe registers. */
    void allocatePayloadRegs(void);
    /*! Create a Gen register from a register set in the payload */
    void allocatePayloadReg(ir::Register, uint32_t offset, uint32_t subOffset = 0);
    /*! Create the intervals for each register */
    /*! Allocate the vectors detected in the instruction selection pass */
    void allocateVector(Selection &selection);
    /*! Allocate the given interval. Return true if success */
    bool createGenReg(const GenRegInterval &interval);
    /*! Indicate if the registers are already allocated in vectors */
    bool isAllocated(const SelectionVector *vector) const;
    /*! Reallocate registers if needed to make the registers in the vector
     *  contigous in memory
     */
    void coalesce(Selection &selection, SelectionVector *vector);
    /*! The context owns the register allocator */
    GenContext &ctx;
    /*! Map virtual registers to offset in the (physical) register file */
    map<ir::Register, uint32_t> RA;
    /*! Provides the position of each register in a vector */
    map<ir::Register, VectorLocation> vectorMap;
    /*! All vectors used in the selection */
    vector<SelectionVector*> vectors;
    /*! The set of booleans that will go to GRF (cannot be kept into flags) */
    set<ir::Register> grfBooleans;
    /*! All the register intervals */
    vector<GenRegInterval> intervals;
    /*! Intervals sorting based on starting point positions */
    vector<GenRegInterval*> starting;
    /*! Intervals sorting based on ending point positions */
    vector<GenRegInterval*> ending;
    /*! registers that are spilled */
    set<ir::Register> spilled;
    /* reserved registers for register spill/reload */
    uint32_t reservedReg;
    /*! Current vector to expire */
    uint32_t expiringID;
    /*! Use custom allocator */
    GBE_CLASS(Opaque);
  };

  // Note that byte vector registers use two bytes per byte (and can be
  // interleaved)
  static const size_t familyVectorSize[] = {2,2,2,4,8};
  static const size_t familyScalarSize[] = {2,1,2,4,8};

  /*! Interval as used in linear scan allocator. Basically, stores the first and
   *  the last instruction where the register is alive
   */
  struct GenRegInterval {
    INLINE GenRegInterval(ir::Register reg) :
      reg(reg), minID(INT_MAX), maxID(-INT_MAX) {}
    ir::Register reg;     //!< (virtual) register of the interval
    int32_t minID, maxID; //!< Starting and ending points
  };

  GenRegAllocator::Opaque::Opaque(GenContext &ctx) : ctx(ctx) {}
  GenRegAllocator::Opaque::~Opaque(void) {}

  void GenRegAllocator::Opaque::allocatePayloadReg(ir::Register reg,
                                                   uint32_t offset,
                                                   uint32_t subOffset)
  {
    using namespace ir;
    assert(offset >= GEN_REG_SIZE);
    offset += subOffset;
    RA.insert(std::make_pair(reg, offset));
    GBE_ASSERT(reg != ocl::blockip || (offset % GEN_REG_SIZE == 0));
    this->intervals[reg].minID = 0;
    this->intervals[reg].maxID = 0;
  }

  INLINE void GenRegAllocator::Opaque::allocatePayloadRegs(void) {
    using namespace ir;
    for(auto &it : this->ctx.curbeRegs)
      allocatePayloadReg(it.first, it.second);

    // Allocate all pushed registers (i.e. structure kernel arguments)
    const Function &fn = ctx.getFunction();
    GBE_ASSERT(fn.getProfile() == PROFILE_OCL);
    const Function::PushMap &pushMap = fn.getPushMap();
    for (auto rit = pushMap.rbegin(); rit != pushMap.rend(); ++rit) {
      const uint32_t argID = rit->second.argID;
      const FunctionArgument arg = fn.getArg(argID);

      const uint32_t subOffset = rit->second.offset;
      const Register reg = rit->second.getRegister();
      auto it = this->ctx.curbeRegs.find(arg.reg);
      assert(it != ctx.curbeRegs.end());
      allocatePayloadReg(reg, it->second, subOffset);
      ctx.splitBlock(it->second, subOffset);
    }
  }

  bool GenRegAllocator::Opaque::createGenReg(const GenRegInterval &interval) {
    using namespace ir;
    const ir::Register reg = interval.reg;
    const uint32_t simdWidth = ctx.getSimdWidth();
    if (RA.contains(reg) == true)
      return true; // already allocated
    GBE_ASSERT(ctx.isScalarReg(reg) == false);
    const bool isScalar = ctx.sel->isScalarOrBool(reg);
    const RegisterData regData = ctx.sel->getRegisterData(reg);
    const RegisterFamily family = regData.family;
    const uint32_t typeSize = isScalar ? familyScalarSize[family] : familyVectorSize[family];
    const uint32_t regSize = isScalar ? typeSize : simdWidth*typeSize;
    uint32_t grfOffset;
    while ((grfOffset = ctx.allocate(regSize, regSize)) == 0) {
      const bool success = this->expireGRF(interval);
      if (UNLIKELY(success == false)) return false;
    }
    GBE_ASSERTM(grfOffset != 0, "Unable to register allocate");
    RA.insert(std::make_pair(reg, grfOffset));
    return true;
  }

  bool GenRegAllocator::Opaque::isAllocated(const SelectionVector *vector) const {
    const ir::Register first = vector->reg[0].reg();
    const auto it = vectorMap.find(first);

    // If the first register is not allocated we are done
    if (it == vectorMap.end())
      return false;

    // If there are more left registers than in the found vector, there are
    // still registers to allocate
    const SelectionVector *other = it->second.first;
    const uint32_t otherFirst = it->second.second;
    const uint32_t leftNum = other->regNum - otherFirst;
    if (leftNum < vector->regNum)
      return false;

    // Now check that all the registers in the already allocated vector match
    // the current vector
    for (uint32_t regID = 1; regID < vector->regNum; ++regID) {
       const ir::Register from = vector->reg[regID].reg();
       const ir::Register to = other->reg[regID + otherFirst].reg();
       if (from != to)
         return false;
    }
    return true;
  }

  void GenRegAllocator::Opaque::coalesce(Selection &selection, SelectionVector *vector) {
    for (uint32_t regID = 0; regID < vector->regNum; ++regID) {
      const ir::Register reg = vector->reg[regID].reg();
      const auto it = this->vectorMap.find(reg);
      // case 1: the register is not already in a vector, so it can stay in this
      // vector. Note that local IDs are *non-scalar* special registers but will
      // require a MOV anyway since pre-allocated in the CURBE
      if (it == vectorMap.end() &&
          ctx.sel->isScalarOrBool(reg) == false &&
          ctx.isSpecialReg(reg) == false)
      {
        const VectorLocation location = std::make_pair(vector, regID);
        this->vectorMap.insert(std::make_pair(reg, location));
      }
      // case 2: the register is already in another vector, so we need to move
      // it to a temporary register.
      // TODO: we can do better than that if we analyze the liveness of the
      // already allocated registers in the vector.  If there is no inteference
      // and the order is maintained, we can reuse the previous vector and avoid
      // the MOVs
      else {
        ir::Register tmp;
        if (vector->isSrc)
          tmp = selection.replaceSrc(vector->insn, regID);
        else
          tmp = selection.replaceDst(vector->insn, regID);
        const VectorLocation location = std::make_pair(vector, regID);
        this->vectorMap.insert(std::make_pair(tmp, location));
      }
    }
  }

  /*! Will sort vector in decreasing order */
  inline bool cmp(const SelectionVector *v0, const SelectionVector *v1) {
    return v0->regNum > v1->regNum;
  }

  void GenRegAllocator::Opaque::allocateVector(Selection &selection) {
    const uint32_t vectorNum = selection.getVectorNum();
    this->vectors.resize(vectorNum);

    // First we find and store all vectors
    uint32_t vectorID = 0;
    for (auto &block : *selection.blockList)
      for (auto &v : block.vectorList)
        this->vectors[vectorID++] = &v;
    GBE_ASSERT(vectorID == vectorNum);

    // Heuristic (really simple...): sort them by the number of registers they
    // contain
    std::sort(this->vectors.begin(), this->vectors.end(), cmp);

    // Insert MOVs when this is required
    for (vectorID = 0; vectorID < vectorNum; ++vectorID) {
      SelectionVector *vector = this->vectors[vectorID];
      if (this->isAllocated(vector))
        continue;
      this->coalesce(selection, vector);
    }
  }

  template <bool sortStartingPoint>
  inline bool cmp(const GenRegInterval *i0, const GenRegInterval *i1) {
    return sortStartingPoint ? i0->minID < i1->minID : i0->maxID < i1->maxID;
  }

  bool GenRegAllocator::Opaque::expireGRF(const GenRegInterval &limit) {
    bool ret = false;
    while (this->expiringID != ending.size()) {
      const GenRegInterval *toExpire = this->ending[this->expiringID];
      const ir::Register reg = toExpire->reg;

      // Dead code produced by the insn selection -> we skip it
      if (toExpire->minID > toExpire->maxID) {
        this->expiringID++;
        continue;
      }

      //ignore register that already spilled
      if(spilled.contains(reg)) {
        this->expiringID++;
        continue;
      }
      // Ignore booleans that were allocated with flags
      if (ctx.sel->getRegisterFamily(reg) == ir::FAMILY_BOOL && !grfBooleans.contains(reg)) {
        this->expiringID++;
        continue;
      }

      if (toExpire->maxID >= limit.minID)
        break;
      auto it = RA.find(reg);
      GBE_ASSERT(it != RA.end());
      // offset less than 32 means it is not managed by our reg allocator.
      if (it->second < 32) {
        this->expiringID++;
        continue;
      }
      // Case 1 - it does not belong to a vector. Just remove it
        ctx.deallocate(it->second);
        this->expiringID++;
        ret = true;
    }

    // We were not able to expire anything
    return ret;
  }

  void GenRegAllocator::Opaque::allocateFlags(Selection &selection) {

    // Store the registers allocated in the map
    map<ir::Register, uint32_t> allocatedFlags;
    GenRegInterval spill = ir::Register(ir::RegisterFile::MAX_INDEX);

    // we have two flags we use for booleans f1.0 and f1.1
    const uint32_t flagNum = 2;
    uint32_t freeFlags[] = {0,1};
    uint32_t freeNum = flagNum;

    // Perform the linear scan allocator on the flag registers only. We only use
    // two flags registers for the booleans right now: f1.0 and f1.1 
    const uint32_t regNum = ctx.sel->getRegNum();
    uint32_t endID = 0; // interval to expire
    for (uint32_t startID = 0; startID < regNum; ++startID) {
      const GenRegInterval &interval = *this->starting[startID];
      const ir::Register reg = interval.reg;
      if (ctx.sel->getRegisterFamily(reg) != ir::FAMILY_BOOL)
        continue; // Not a flag. We don't care
      if (grfBooleans.contains(reg))
        continue; // Cannot use a flag register
      if (interval.maxID == -INT_MAX)
        continue; // Unused register
      if (freeNum != 0) {
        spill = interval;
        allocatedFlags.insert(std::make_pair(reg, freeFlags[--freeNum]));
      }
      else {
        // Try to expire one register
        while (endID != ending.size()) {
          const GenRegInterval *toExpire = this->ending[endID];
          const ir::Register reg = toExpire->reg;
          // Dead code produced by the insn selection -> we skip it
          if (toExpire->minID > toExpire->maxID) {
            endID++;
            continue;
          }
          // We cannot expire this interval and the next ones
          if (toExpire->maxID >= interval.minID)
            break;
          // Must be a boolean allocated with a flag register
          if (ctx.sel->getRegisterFamily(reg) != ir::FAMILY_BOOL || grfBooleans.contains(reg)) {
            endID++;
            continue;
          }
          // We reuse a flag from a previous interval (the oldest one)
          auto it = allocatedFlags.find(toExpire->reg);
          GBE_ASSERT(it != allocatedFlags.end());
          freeFlags[freeNum++] = it->second;
          endID++;
          break;
        }

        // We need to spill one of the previous boolean values
        if (freeNum == 0) {
          GBE_ASSERT(uint16_t(spill.reg) != ir::RegisterFile::MAX_INDEX);
          // We spill the last inserted boolean and use its flag instead for
          // this one
          if (spill.maxID > interval.maxID) {
            auto it = allocatedFlags.find(spill.reg);
            GBE_ASSERT(it != allocatedFlags.end());
            allocatedFlags.insert(std::make_pair(reg, it->second));
            allocatedFlags.erase(spill.reg);
            grfBooleans.insert(spill.reg);
            spill = interval;
          }
          // We will use a grf for the current register
          else {
            grfBooleans.insert(reg);
          }
        }
        else
          allocatedFlags.insert(std::make_pair(reg, freeFlags[--freeNum]));
      }
    }

    // Now, we traverse all the selection instructions and we patch them to make
    // them use flag registers
    for (auto &block : *selection.blockList)
    for (auto &insn : block.insnList) {
      const uint32_t srcNum = insn.srcNum, dstNum = insn.dstNum;

      // Patch the source booleans
      for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
        const GenRegister selReg = insn.src(srcID);
        const ir::Register reg = selReg.reg();
        if (selReg.physical || ctx.sel->getRegisterFamily(reg) != ir::FAMILY_BOOL)
          continue;
        auto it = allocatedFlags.find(reg);
        if (it == allocatedFlags.end())
          continue;
        // Use a flag register for it now
        insn.src(srcID) = GenRegister::flag(1,it->second);
      }

      // Patch the destination booleans
      for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
        const GenRegister selReg = insn.dst(dstID);
        const ir::Register reg = selReg.reg();
        if (selReg.physical || ctx.sel->getRegisterFamily(reg) != ir::FAMILY_BOOL)
          continue;
        auto it = allocatedFlags.find(reg);
        if (it == allocatedFlags.end())
          continue;
        // Use a flag register for it now
        insn.dst(dstID) = GenRegister::flag(1,it->second);
      }

      // Patch the predicate now. Note that only compares actually modify it (it
      // is called a "conditional modifier"). The other instructions just read
      // it
      if (insn.state.physicalFlag == 0) {
        auto it = allocatedFlags.find(ir::Register(insn.state.flagIndex));
        // Just patch it if we can use a flag directly
        if (it != allocatedFlags.end()) {
          insn.state.flag = 1;
          insn.state.subFlag = it->second;
          insn.state.physicalFlag = 1;
        }
        // When we let the boolean in a GRF, use f0.1 as a temporary
        else {
          // Mov the GRF to the flag such that the flag can be read
          SelectionInstruction *mov0 = selection.create(SEL_OP_MOV,1,1);
          mov0->state = GenInstructionState(1);
          mov0->state.predicate = GEN_PREDICATE_NONE;
          mov0->state.noMask = 1;
          mov0->src(0) = GenRegister::uw1grf(ir::Register(insn.state.flagIndex));
          mov0->dst(0) = GenRegister::flag(0,1);

          // Do not prepend if the flag is not read (== used only as a
          // conditional modifier)
          if (insn.state.predicate != GEN_PREDICATE_NONE)
            insn.prepend(*mov0);

          // We can use f0.1 (our "backdoor" flag)
          insn.state.flag = 0;
          insn.state.subFlag = 1;
          insn.state.physicalFlag = 1;

          // Compare instructions update the flags so we must copy it back to
          // the GRF
          if (insn.opcode == SEL_OP_CMP || insn.opcode == SEL_OP_I64CMP) {
            SelectionInstruction *mov1 = selection.create(SEL_OP_MOV,1,1);
            mov1->state = mov0->state;
            mov1->dst(0) = mov0->src(0);
            mov1->src(0) = mov0->dst(0);
            insn.append(*mov1);
          }
        }
      }
    }
  }

  bool GenRegAllocator::Opaque::allocateGRFs(Selection &selection) {
    // Perform the linear scan allocator
    const uint32_t regNum = ctx.sel->getRegNum();
    for (uint32_t startID = 0; startID < regNum; ++startID) {
      const GenRegInterval &interval = *this->starting[startID];
      const ir::Register reg = interval.reg;
      if (interval.maxID == -INT_MAX)
        continue; // Unused register
      if (RA.contains(reg))
        continue; // already allocated

      if (ctx.sel->getRegisterFamily(reg) == ir::FAMILY_BOOL && !grfBooleans.contains(reg))
        continue;

      // Case 1: the register belongs to a vector, allocate all the registers in
      // one piece
      auto it = vectorMap.find(reg);
      if (it != vectorMap.end()) {
        const SelectionVector *vector = it->second.first;
        // all the reg in the SelectionVector are spilled
        if(spilled.contains(vector->reg[0].reg()))
          continue;
        const uint32_t simdWidth = ctx.getSimdWidth();

        const ir::RegisterData regData = ctx.sel->getRegisterData(reg);
        const ir::RegisterFamily family = regData.family;
        const uint32_t typeSize = familyVectorSize[family];
        const uint32_t alignment = simdWidth*typeSize;

        const uint32_t size = vector->regNum * alignment;

        uint32_t grfOffset;
        while ((grfOffset = ctx.allocate(size, alignment)) == 0) {
          const bool success = this->expireGRF(interval);
          if (success == false) {
            // if no spill support, just return false, else simply spill the register
            if(reservedReg == 0) return false;
            break;
          }
        }
        if(grfOffset == 0) {
          // spill all the registers in the SelectionVector
          // the tricky here is I need to use reservedReg+1 as scratch write payload.
          // so, i need to write the first register to scratch memory first.
          // the spillReg() will just append scratch write insn after the def. To spill
          // the first register, need to call spillReg() last for the vector->reg[0]
          GBE_ASSERT(vector->regNum < RESERVED_REG_NUM_FOR_SPILL);
          for(int i = vector->regNum-1; i >= 0; i--) {
            spilled.insert(vector->reg[i].reg());
            selection.spillReg(vector->reg[i].reg(), reservedReg);
          }
          continue;
        }
        for (uint32_t regID = 0; regID < vector->regNum; ++regID) {
          const ir::Register reg = vector->reg[regID].reg();
          GBE_ASSERT(RA.contains(reg) == false
                     && ctx.sel->getRegisterData(reg).family == family);
          RA.insert(std::make_pair(reg, grfOffset + alignment * regID));
          ctx.splitBlock(grfOffset, alignment * regID);  //splitBlock will not split if regID == 0
        }
      }
      // Case 2: This is a regular scalar register, allocate it alone
      else if (this->createGenReg(interval) == false) {
        if(reservedReg == 0) return false;
        spilled.insert(reg);
        selection.spillReg(reg, reservedReg);
      }
    }
    return true;
  }

  INLINE bool GenRegAllocator::Opaque::allocate(Selection &selection) {
    using namespace ir;
    if (ctx.getSimdWidth() == 8) {
      reservedReg = ctx.allocate(RESERVED_REG_NUM_FOR_SPILL * GEN_REG_SIZE, GEN_REG_SIZE);
      reservedReg /= GEN_REG_SIZE;
    } else {
      reservedReg = 0;
    }
    // Allocate all the vectors first since they need to be contiguous
    this->allocateVector(selection);
    // schedulePreRegAllocation(ctx, selection);

    // Now start the linear scan allocation
    for (uint32_t regID = 0; regID < ctx.sel->getRegNum(); ++regID)
      this->intervals.push_back(ir::Register(regID));

    // Allocate the special registers (only those which are actually used)
    this->allocatePayloadRegs();

    // Group and barrier IDs are always allocated by the hardware in r0
    RA.insert(std::make_pair(ocl::groupid0,  1*sizeof(float))); // r0.1
    RA.insert(std::make_pair(ocl::groupid1,  6*sizeof(float))); // r0.6
    RA.insert(std::make_pair(ocl::groupid2,  7*sizeof(float))); // r0.7
    RA.insert(std::make_pair(ocl::barrierid, 2*sizeof(float))); // r0.2

    // block IP used to handle the mask in SW is always allocated

    // Compute the intervals
    int32_t insnID = 0;
    for (auto &block : *selection.blockList) {
      int32_t lastID = insnID;
      int32_t firstID = insnID;
      // Update the intervals of each used register. Note that we do not
      // register allocate R0, so we skip all sub-registers in r0
      for (auto &insn : block.insnList) {
        const uint32_t srcNum = insn.srcNum, dstNum = insn.dstNum;
        for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
          const GenRegister &selReg = insn.src(srcID);
          const ir::Register reg = selReg.reg();
          if (selReg.file != GEN_GENERAL_REGISTER_FILE ||
              reg == ir::ocl::barrierid ||
              reg == ir::ocl::groupid0  ||
              reg == ir::ocl::groupid1  ||
              reg == ir::ocl::groupid2)
            continue;
          this->intervals[reg].minID = std::min(this->intervals[reg].minID, insnID);
          this->intervals[reg].maxID = std::max(this->intervals[reg].maxID, insnID);
        }
        for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
          const GenRegister &selReg = insn.dst(dstID);
          const ir::Register reg = selReg.reg();
          if (selReg.file != GEN_GENERAL_REGISTER_FILE ||
              reg == ir::ocl::barrierid ||
              reg == ir::ocl::groupid0 ||
              reg == ir::ocl::groupid1 ||
              reg == ir::ocl::groupid2)
            continue;
          this->intervals[reg].minID = std::min(this->intervals[reg].minID, insnID);
          this->intervals[reg].maxID = std::max(this->intervals[reg].maxID, insnID);
        }

        // Flag registers can only go to src[0]
        const SelectionOpcode opcode = SelectionOpcode(insn.opcode);
        if (opcode == SEL_OP_AND || opcode == SEL_OP_OR || opcode == SEL_OP_XOR
            || opcode == SEL_OP_I64AND || opcode == SEL_OP_I64OR || opcode == SEL_OP_I64XOR) {
          if (insn.src(1).physical == 0) {
            const ir::Register reg = insn.src(1).reg();
            if (ctx.sel->getRegisterFamily(reg) == ir::FAMILY_BOOL)
              grfBooleans.insert(reg);
          }
        }

        // OK, a flag is used as a predicate or a conditional modifier
        if (insn.state.physicalFlag == 0) {
          const ir::Register reg = ir::Register(insn.state.flagIndex);
          this->intervals[reg].minID = std::min(this->intervals[reg].minID, insnID);
          this->intervals[reg].maxID = std::max(this->intervals[reg].maxID, insnID);
        }
        lastID = insnID;
        insnID++;
      }

      // All registers alive at the begining of the block must update their intervals.
      const ir::BasicBlock *bb = block.bb;
      for (auto reg : ctx.getLiveIn(bb))
          this->intervals[reg].minID = std::min(this->intervals[reg].minID, firstID);

      for (auto reg : ctx.getExtraLiveIn(bb))
          this->intervals[reg].minID = std::min(this->intervals[reg].minID, firstID);
      // All registers alive at the end of the block must have their intervals
      // updated as well
      for (auto reg : ctx.getLiveOut(bb))
        this->intervals[reg].maxID = std::max(this->intervals[reg].maxID, lastID);

      for (auto reg : ctx.getExtraLiveOut(bb))
        this->intervals[reg].maxID = std::max(this->intervals[reg].maxID, lastID);
    }

    this->intervals[ocl::emask].minID = 0;
    this->intervals[ocl::emask].maxID = INT_MAX;
    this->intervals[ocl::notemask].minID = 0;
    this->intervals[ocl::notemask].maxID = INT_MAX;
    this->intervals[ocl::retVal].minID = INT_MAX;
    this->intervals[ocl::retVal].maxID = -INT_MAX;

    // Sort both intervals in starting point and ending point increasing orders
    const uint32_t regNum = ctx.sel->getRegNum();
    this->starting.resize(regNum);
    this->ending.resize(regNum);
    for (uint32_t regID = 0; regID < regNum; ++regID)
      this->starting[regID] = this->ending[regID] = &intervals[regID];
    std::sort(this->starting.begin(), this->starting.end(), cmp<true>);
    std::sort(this->ending.begin(), this->ending.end(), cmp<false>);

    // Remove the registers that were not allocated
    this->expiringID = 0;
    while (this->expiringID < regNum) {
      const GenRegInterval *interval = ending[this->expiringID];
      if (interval->maxID == -INT_MAX)
        this->expiringID++;
      else
        break;
    }

    // First we try to put all booleans registers into flags
    this->allocateFlags(selection);

    // Allocate all the GRFs now (regular register and boolean that are not in
    // flag registers)
    return this->allocateGRFs(selection);
  }

  INLINE void GenRegAllocator::Opaque::outputAllocation(void) {
    using namespace std;
    cout << "## register allocation ##" << endl;
    for(auto &i : RA) {
        ir::Register vReg = (ir::Register)i.first;
        int offst = (int)i.second;// / sizeof(float);
        ir::RegisterData regData = ctx.sel->getRegisterData(vReg);
        int reg = offst / 32;
        int subreg = offst % 32;
        ir::RegisterFamily family = regData.family;
        int registerSize;
        if (family == ir::FAMILY_BOOL)
          registerSize = 2;
        else {
          registerSize = ir::getFamilySize(regData.family);
          if (!ctx.isScalarReg(vReg))
            registerSize *= ctx.getSimdWidth();
        }
        cout << "%" << setiosflags(ios::left) << setw(8) << vReg << "g"
             << setiosflags(ios::left) << setw(3) << reg << "."
             << setiosflags(ios::left) << setw(2) << subreg
             <<  "  " << setw(3) << registerSize << "B"
             << "  [" << setw(8) << this->intervals[(uint)vReg].minID
             << " -> " << setw(8) << this->intervals[(uint)vReg].maxID
             << "]" << endl;
    }
    std::set<ir::Register>::iterator is;
    std::cout << "## spilled registers:" << std::endl;
    for(is = spilled.begin(); is != spilled.end(); is++)
      std::cout << (int)*is << std::endl;
    std::cout << std::endl;
  }

  INLINE GenRegister setGenReg(const GenRegister &src, uint32_t grfOffset) {
    GenRegister dst;
    dst = src;
    dst.physical = 1;
    dst.nr = grfOffset / GEN_REG_SIZE;
    dst.subnr = grfOffset % GEN_REG_SIZE;
    return dst;
  }

  INLINE GenRegister GenRegAllocator::Opaque::genReg(const GenRegister &reg) {
    if (reg.file == GEN_GENERAL_REGISTER_FILE) {
      if(reg.physical == 1) {
        return reg;
      }
      GBE_ASSERT(RA.contains(reg.reg()) != false);
      const uint32_t grfOffset = RA.find(reg.reg())->second;
      const uint32_t suboffset = reg.subphysical ? reg.subnr : 0;
      const GenRegister dst = setGenReg(reg, grfOffset + suboffset);
      if (reg.quarter != 0)
        return GenRegister::Qn(dst, reg.quarter);
      else
        return dst;
    }
    else
      return reg;
  }

  /////////////////////////////////////////////////////////////////////////////
  // Register allocator public implementation
  /////////////////////////////////////////////////////////////////////////////

  GenRegAllocator::GenRegAllocator(GenContext &ctx) {
    this->opaque = GBE_NEW(GenRegAllocator::Opaque, ctx);
  }

  GenRegAllocator::~GenRegAllocator(void) {
    GBE_DELETE(this->opaque);
  }

  bool GenRegAllocator::allocate(Selection &selection) {
    return this->opaque->allocate(selection);
  }

  GenRegister GenRegAllocator::genReg(const GenRegister &reg) {
    return this->opaque->genReg(reg);
  }

  void GenRegAllocator::outputAllocation(void) {
    this->opaque->outputAllocation();
  }

} /* namespace gbe */

