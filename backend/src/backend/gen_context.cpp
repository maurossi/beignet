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
 * \file gen_context.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */

#include "backend/gen_context.hpp"
#include "backend/gen_program.hpp"
#include "backend/gen_defs.hpp"
#include "backend/gen_eu.hpp"
#include "ir/function.hpp"
#include "sys/cvar.hpp"
#include <cstring>

namespace gbe
{
  GenContext::GenContext(const ir::Unit &unit, const std::string &name) :
    Context(unit, name)
  {
    p = GBE_NEW(GenEmitter, simdWidth, 7); // XXX handle more than gen7
  }


  GenContext::~GenContext(void) { GBE_DELETE(p); }

  void GenContext::allocatePayloadReg(gbe_curbe_type value,
                                      uint32_t subValue,
                                      const ir::Register &reg)
  {
    const int32_t offset = kernel->getCurbeOffset(value, subValue);
    if (offset >= 0) {
      const ir::RegisterData data = fn.getRegisterData(reg);
      const uint32_t typeSize = data.getSize();
      const uint32_t nr = (offset + GEN_REG_SIZE) / GEN_REG_SIZE;
      const uint32_t subnr = ((offset + GEN_REG_SIZE) % GEN_REG_SIZE) / typeSize;
      GBE_ASSERT(data.family == ir::FAMILY_DWORD); // XXX support the rest
      if (this->isScalarReg(reg) == true)
        RA.insert(std::make_pair(reg, GenReg::vec1(GEN_GENERAL_REGISTER_FILE, nr, subnr)));
      else if (this->simdWidth == 8)
        RA.insert(std::make_pair(reg, GenReg::vec8(GEN_GENERAL_REGISTER_FILE, nr, subnr)));
      else if (this->simdWidth == 16)
        RA.insert(std::make_pair(reg, GenReg::vec16(GEN_GENERAL_REGISTER_FILE, nr, subnr)));
    }
  }

  void GenContext::allocateRegister(void) {
    using namespace ir;
    GBE_ASSERT(fn.getProfile() == PROFILE_OCL);

    // Allocate the special registers (only those which are actually used)
    allocatePayloadReg(GBE_CURBE_LOCAL_ID_X, 0, ocl::lid0);
    allocatePayloadReg(GBE_CURBE_LOCAL_ID_Y, 0, ocl::lid1);
    allocatePayloadReg(GBE_CURBE_LOCAL_ID_Z, 0, ocl::lid2);
    allocatePayloadReg(GBE_CURBE_LOCAL_SIZE_X, 0, ocl::lsize0);
    allocatePayloadReg(GBE_CURBE_LOCAL_SIZE_Y, 0, ocl::lsize1);
    allocatePayloadReg(GBE_CURBE_LOCAL_SIZE_Z, 0, ocl::lsize2);
    allocatePayloadReg(GBE_CURBE_GLOBAL_SIZE_X, 0, ocl::gsize0);
    allocatePayloadReg(GBE_CURBE_GLOBAL_SIZE_Y, 0, ocl::gsize1);
    allocatePayloadReg(GBE_CURBE_GLOBAL_SIZE_Z, 0, ocl::gsize2);
    allocatePayloadReg(GBE_CURBE_GLOBAL_OFFSET_X, 0, ocl::goffset0);
    allocatePayloadReg(GBE_CURBE_GLOBAL_OFFSET_Y, 0, ocl::goffset1);
    allocatePayloadReg(GBE_CURBE_GLOBAL_OFFSET_Z, 0, ocl::goffset2);
    allocatePayloadReg(GBE_CURBE_GROUP_NUM_X, 0, ocl::numgroup0);
    allocatePayloadReg(GBE_CURBE_GROUP_NUM_Y, 0, ocl::numgroup1);
    allocatePayloadReg(GBE_CURBE_GROUP_NUM_Z, 0, ocl::numgroup2);

    // Group IDs are always allocated by the hardware in r0
    RA.insert(std::make_pair(ocl::groupid0, GenReg::vec1(GEN_GENERAL_REGISTER_FILE, 0, 1)));
    RA.insert(std::make_pair(ocl::groupid1, GenReg::vec1(GEN_GENERAL_REGISTER_FILE, 0, 6)));
    RA.insert(std::make_pair(ocl::groupid2, GenReg::vec1(GEN_GENERAL_REGISTER_FILE, 0, 7)));

    // Allocate all input parameters
    const uint32_t inputNum = fn.inputNum();
    for (uint32_t inputID = 0; inputID < inputNum; ++inputID) {
      const FunctionInput &input = fn.getInput(inputID);
      GBE_ASSERT(input.type == FunctionInput::GLOBAL_POINTER ||
                 input.type == FunctionInput::CONSTANT_POINTER);
      allocatePayloadReg(GBE_CURBE_KERNEL_ARGUMENT, inputID, input.reg);
    }

    // First we build the set of all used registers
    set<Register> usedRegs;
    fn.foreachInstruction([&usedRegs](const Instruction &insn) {
      const uint32_t srcNum = insn.getSrcNum(), dstNum = insn.getDstNum();
      for (uint32_t srcID = 0; srcID < srcNum; ++srcID)
        usedRegs.insert(insn.getSrc(srcID));
      for (uint32_t dstID = 0; dstID < dstNum; ++dstID)
        usedRegs.insert(insn.getDst(dstID));
    });

    // Allocate all used registers. Just crash when we run out-of-registers
    // r0 is always taken by the HW. We also always write down local IDs after
    // the curbe data
    uint32_t grfOffset = kernel->getCurbeSize() + GEN_REG_SIZE
                       + 3 * sizeof(uint32_t) * this->simdWidth;
    GBE_ASSERT(simdWidth != 32); // XXX a bit more complicated see later
    if (simdWidth == 16) grfOffset = ALIGN(grfOffset, 64);
    for (auto reg : usedRegs) {
      if (fn.isSpecialReg(reg) == true) continue; // already done
      if (fn.getInput(reg) != NULL) continue; // already done
      const RegisterData regData = fn.getRegisterData(reg);
      const RegisterFamily family = regData.family;
      const uint32_t typeSize = regData.getSize();
      const uint32_t nr = grfOffset / GEN_REG_SIZE;
      const uint32_t subnr = (grfOffset % GEN_REG_SIZE) / typeSize;
      GBE_ASSERT(family == FAMILY_DWORD); // XXX Do the rest
      GBE_ASSERT(grfOffset + simdWidth*typeSize < GEN_GRF_SIZE);
      RA.insert(std::make_pair(reg, GenReg::vec16(GEN_GENERAL_REGISTER_FILE, nr, subnr)));
      grfOffset += simdWidth * typeSize;
    }
  }

  void GenContext::emitUnaryInstruction(const ir::UnaryInstruction &insn) {
    GBE_ASSERT(insn.getOpcode() == ir::OP_MOV);
    p->MOV(reg(insn.getDst(0)), reg(insn.getSrc(0)));
  }

  void GenContext::emitBinaryInstruction(const ir::BinaryInstruction &insn) {
    using namespace ir;
    const Opcode opcode = insn.getOpcode();
    const Type type = insn.getType();
    GenReg dst = reg(insn.getDst(0));
    GenReg src0 = reg(insn.getSrc(0));
    GenReg src1 = reg(insn.getSrc(1));

    // Default type is FLOAT
    GBE_ASSERT(type == TYPE_U32 || type == TYPE_S32 || type == TYPE_FLOAT);
    if (type == TYPE_U32) {
      dst = GenReg::retype(dst, GEN_TYPE_UD);
      src0 = GenReg::retype(src0, GEN_TYPE_UD);
      src1 = GenReg::retype(src1, GEN_TYPE_UD);
    } else if (type == TYPE_S32) {
      dst = GenReg::retype(dst, GEN_TYPE_D);
      src0 = GenReg::retype(src0, GEN_TYPE_D);
      src1 = GenReg::retype(src1, GEN_TYPE_D);
    }

    // Output the binary instruction
    switch (opcode) {
      case OP_ADD: p->ADD(dst, src0, src1); break;
      case OP_MUL: 
      {
        p->MUL(dst, src0, src1);
#if 0
        if (type == TYPE_FLOAT) p->MUL(dst, src0, src1);
        else {

        }
#endif
        break;
      }
      default: NOT_IMPLEMENTED;
    }
  }

  void GenContext::emitTernaryInstruction(const ir::TernaryInstruction &insn) {}
  void GenContext::emitSelectInstruction(const ir::SelectInstruction &insn) {}
  void GenContext::emitCompareInstruction(const ir::CompareInstruction &insn) {}
  void GenContext::emitConvertInstruction(const ir::ConvertInstruction &insn) {}
  void GenContext::emitBranchInstruction(const ir::BranchInstruction &insn) {}
  void GenContext::emitTextureInstruction(const ir::TextureInstruction &insn) {}

  void GenContext::emitLoadImmInstruction(const ir::LoadImmInstruction &insn) {
    using namespace ir;
    const Type type = insn.getType();
    const Immediate imm = insn.getImmediate();
    const GenReg dst = reg(insn.getDst(0));

    switch (type) {
      case TYPE_U32: p->MOV(GenReg::retype(dst, GEN_TYPE_UD), GenReg::immud(imm.data.u32)); break;
      case TYPE_S32: p->MOV(GenReg::retype(dst, GEN_TYPE_D), GenReg::immd(imm.data.s32)); break;
      case TYPE_FLOAT: p->MOV(dst, GenReg::immf(imm.data.f32)); break;
      default: NOT_SUPPORTED;
    }
  }

  void GenContext::emitLoadInstruction(const ir::LoadInstruction &insn) {
    using namespace ir;
    GBE_ASSERT(insn.getAddressSpace() == MEM_GLOBAL);
    GBE_ASSERT(insn.getValueNum() == 1);
    GBE_ASSERT(insn.isAligned() == true);
    GBE_ASSERT(this->simdWidth <= 16);
    const GenReg address = reg(insn.getAddress());
    const GenReg value = reg(insn.getValue(0));
    // XXX remove that later. Now we just copy everything to GRFs to make it
    // contiguous
    if (this->simdWidth == 8 || this->simdWidth == 16)
      p->UNTYPED_READ(value, address, 0, 1);
    else
      NOT_IMPLEMENTED;
  }

  void GenContext::emitStoreInstruction(const ir::StoreInstruction &insn) {
    using namespace ir;
    GBE_ASSERT(insn.getAddressSpace() == MEM_GLOBAL);
    GBE_ASSERT(insn.getValueNum() == 1);
    GBE_ASSERT(insn.isAligned() == true);
    GBE_ASSERT(this->simdWidth <= 16);
    const GenReg address = reg(insn.getAddress());
    const GenReg value = reg(insn.getValue(0));
    // XXX remove that later. Now we just copy everything to GRFs to make it
    // contiguous
    if (this->simdWidth == 8) {
      p->MOV(GenReg::vec8grf(112, 0), GenReg::retype(address, GEN_TYPE_F));
      p->MOV(GenReg::vec8grf(113, 0), GenReg::retype(value, GEN_TYPE_F));
      p->UNTYPED_WRITE(GenReg::vec8grf(112, 0), 0, 1);
    } else if (this->simdWidth == 16) {
      p->MOV(GenReg::vec16grf(112, 0), GenReg::retype(address, GEN_TYPE_F));
      p->MOV(GenReg::vec16grf(114, 0), GenReg::retype(value, GEN_TYPE_F));
      p->UNTYPED_WRITE(GenReg::vec16grf(112, 0), 0, 1);
    } else
      NOT_IMPLEMENTED;
  }
  void GenContext::emitFenceInstruction(const ir::FenceInstruction &insn) {}
  void GenContext::emitLabelInstruction(const ir::LabelInstruction &insn) {}

  void GenContext::emitInstructionStream(void) {
    using namespace ir;
    fn.foreachInstruction([&](const Instruction &insn) {
      const Opcode opcode = insn.getOpcode();
      switch (opcode) {
#define DECL_INSN(OPCODE, FAMILY) \
      case OP_##OPCODE: this->emit##FAMILY(cast<FAMILY>(insn)); break;
#include "ir/instruction.hxx"
#undef DECL_INSN
      }
    });
  }

  BVAR(OCL_OUTPUT_ASM, false);
  void GenContext::emitCode(void) {
    GenKernel *genKernel = static_cast<GenKernel*>(this->kernel);
    this->allocateRegister();
    this->emitInstructionStream();
    p->EOT(127);
    genKernel->insnNum = p->insnNum;
    genKernel->insns = GBE_NEW_ARRAY(GenInstruction, genKernel->insnNum);
    std::memcpy(genKernel->insns, p->store, genKernel->insnNum * sizeof(GenInstruction));
    if (OCL_OUTPUT_ASM) {
      FILE *f = fopen("asm.dump", "wb");
      fwrite(genKernel->insns, 1, genKernel->insnNum * sizeof(GenInstruction), f);
      fclose(f);
    }
  }

  Kernel *GenContext::allocateKernel(void) {
    return GBE_NEW(GenKernel, name);
  }

} /* namespace gbe */

