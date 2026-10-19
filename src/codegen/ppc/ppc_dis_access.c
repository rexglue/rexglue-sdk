/**
 * @file        codegen/ppc/ppc_dis_access.c
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc-dis.c"  // NOLINT(bugprone-suspicious-include): struct powerpc_operand and its extractors exist only inside the vendored file

/**
 * Reports whether every operand extractor of a table entry accepts an instruction word.
 *
 * @param entry The opcode table entry.
 * @param code The instruction word.
 * @param dialect The PPC_OPCODE_* bits being decoded for.
 * @return 1 when no extractor flags the word invalid, otherwise 0.
 */
int AreOperandsValid(const struct powerpc_opcode* entry, unsigned long code, int dialect) {
  int invalid = 0;
  const unsigned char* opindex;
  for (opindex = entry->operands; *opindex != 0; opindex++) {
    const struct powerpc_operand* operand = powerpc_operands + *opindex;
    if (operand->extract)
      (*operand->extract)(code, dialect, &invalid);
  }
  return invalid == 0;
}
