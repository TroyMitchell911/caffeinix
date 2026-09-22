/* SV39 page-table geometry and huge-leaf selection helpers. */
#ifndef __CAFFEINIX_ARCH_RISCV_VM_LAYOUT_H
#define __CAFFEINIX_ARCH_RISCV_VM_LAYOUT_H

#include <typedefs.h>

#define SV39_LEVEL_MAX 2

/**
 * sv39_level_size() - Return bytes mapped by an SV39 leaf level.
 * @level: Hardware level in [0, SV39_LEVEL_MAX].
 *
 * Context: Any context; no state or locks.
 *
 * Return: Leaf size in bytes, or zero for an invalid level.
 */
uint64 sv39_level_size(unsigned int level);
/**
 * sv39_best_map_level() - Choose the largest aligned SV39 leaf level.
 * @virtual: Page-aligned virtual start.
 * @physical: Page-aligned physical start.
 * @size: Remaining mapping bytes.
 *
 * Context: Any context; does not sleep.
 * Return: The largest level whose leaf fits and is aligned. Returns level zero
 * when larger leaves cannot fit; callers must satisfy page alignment first.
 */
int sv39_best_map_level(uint64 virtual, uint64 physical, uint64 size);

#endif
