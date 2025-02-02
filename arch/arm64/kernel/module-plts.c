/*
 * Copyright (C) 2014-2017 Linaro Ltd. <ard.biesheuvel@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <asm-generic/asm-offsets.h>
#include <asm/insn.h>
#include <stdint.h>
extern int read_cpu_memory(void *addr, size_t size);
static int aarch64_insn_decode_register(int regtype, unsigned long addr)
{
    unsigned int insn;

    insn = read_cpu_memory(addr, sizeof(insn));

    switch (regtype) {
    case AARCH64_INSN_REGTYPE_RD:
        return insn >> 12 & 0x1f;
    case AARCH64_INSN_REGTYPE_RS1:
        return insn >> 16 & 0x1f;
    case AARCH64_INSN_REGTYPE_RS2:
        return insn >> 20 & 0x1f;
    default:
        return -1;
    }
}

static struct aarch64_insn aarch64_insn_gen_movewide(int rd, u16 val, int variant, int size)
{
    struct aarch64_insn insn;

    insn.bits.opcode = AARCH64_INSN_OP_MOVWIDE;
    insn.bits.rd = rd;
    insn.bits.variant = variant;
    insn.bits.size = size;
    insn.bits.imm = val;

    return insn;
}

static struct aarch64_insn aarch64_insn_gen_branch_imm(u64 addr, u64 imm, int link)
{
    struct aarch64_insn insn;

    insn.bits.opcode = AARCH64_INSN_OP_BRANCH;
    insn.bits.imm = imm;
    insn.bits.addr = addr;
    insn.bits.link = link;

    return insn;
}

#include <linux/elf.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sort.h>

static bool in_init(const struct module *mod, void *loc)
{
	return (u64)loc - (u64)mod->init_layout.base < mod->init_layout.size;
}

u64 module_emit_plt_entry(struct module *mod, void *loc, const Elf64_Rela *rela,
			  Elf64_Sym *sym)
{
	struct mod_plt_sec *pltsec = !in_init(mod, loc) ? &mod->arch.core :
							  &mod->arch.init;
	struct plt_entry *plt = (struct plt_entry *)pltsec->plt->sh_addr;
	int i = pltsec->plt_num_entries;
	u64 val = sym->st_value + rela->r_addend;

	plt[i] = get_plt_entry(val);

	/*
	 * Check if the entry we just created is a duplicate. Given that the
	 * relocations are sorted, this will be the last entry we allocated.
	 * (if one exists).
	 */
	if (i > 0 && plt_entries_equal(plt + i, plt + i - 1))
		return (u64)&plt[i - 1];

	pltsec->plt_num_entries++;
	if (WARN_ON(pltsec->plt_num_entries > pltsec->plt_max_entries))
		return 0;

	return (u64)&plt[i];
}

#ifdef CONFIG_ARM64_ERRATUM_843419
u64 module_emit_veneer_for_adrp(struct module *mod, void *loc, u64 val)
{
	struct mod_plt_sec *pltsec = !in_init(mod, loc) ? &mod->arch.core :
							  &mod->arch.init;
	struct plt_entry *plt = (struct plt_entry *)pltsec->plt->sh_addr;
	int i = pltsec->plt_num_entries++;
	u32 mov0, mov1, mov2, br;
	int rd;

	if (WARN_ON(pltsec->plt_num_entries > pltsec->plt_max_entries))
		return 0;

	/* get the destination register of the ADRP instruction */
	rd = aarch64_insn_decode_register(AARCH64_INSN_REGTYPE_RD,
					  le32_to_cpup((__le32 *)loc));

	/* generate the veneer instructions */
	mov0 = aarch64_insn_gen_movewide(rd, (u16)~val, 0,
					 AARCH64_INSN_VARIANT_64BIT,
					 AARCH64_INSN_MOVEWIDE_INVERSE);
	mov1 = aarch64_insn_gen_movewide(rd, (u16)(val >> 16), 16,
					 AARCH64_INSN_VARIANT_64BIT,
					 AARCH64_INSN_MOVEWIDE_KEEP);
	mov2 = aarch64_insn_gen_movewide(rd, (u16)(val >> 32), 32,
					 AARCH64_INSN_VARIANT_64BIT,
					 AARCH64_INSN_MOVEWIDE_KEEP);
	br = aarch64_insn_gen_branch_imm((u64)&plt[i].br, (u64)loc + 4,
					 AARCH64_INSN_BRANCH_NOLINK);

	plt[i] = (struct plt_entry){
			cpu_to_le32(mov0),
			cpu_to_le32(mov1),
			cpu_to_le32(mov2),
			cpu_to_le32(br)
		};

	return (u64)&plt[i];
}
#endif

#define cmp_3way(a,b)	((a) < (b) ? -1 : (a) > (b))

static int cmp_rela(const void *a, const void *b)
{
	const Elf64_Rela *x = a, *y = b;
	int i;

	/* sort by type, symbol index and addend */
	i = cmp_3way(ELF64_R_TYPE(x->r_info), ELF64_R_TYPE(y->r_info));
	if (i == 0)
		i = cmp_3way(ELF64_R_SYM(x->r_info), ELF64_R_SYM(y->r_info));
	if (i == 0)
		i = cmp_3way(x->r_addend, y->r_addend);
	return i;
}

static bool duplicate_rel(const Elf64_Rela *rela, int num)
{
	/*
	 * Entries are sorted by type, symbol index and addend. That means
	 * that, if a duplicate entry exists, it must be in the preceding
	 * slot.
	 */
	return num > 0 && cmp_rela(rela + num, rela + num - 1) == 0;
}

static unsigned int count_plts(Elf64_Sym *syms, Elf64_Rela *rela, int num,
			       Elf64_Word dstidx, Elf_Shdr *dstsec)
{
	unsigned int ret = 0;
	Elf64_Sym *s;
	int i;

	for (i = 0; i < num; i++) {
		u64 min_align;

		switch (ELF64_R_TYPE(rela[i].r_info)) {
		case R_AARCH64_JUMP26:
		case R_AARCH64_CALL26:
			if (!IS_ENABLED(CONFIG_RANDOMIZE_BASE))
				break;

			/*
			 * We only have to consider branch targets that resolve
			 * to symbols that are defined in a different section.
			 * This is not simply a heuristic, it is a fundamental
			 * limitation, since there is no guaranteed way to emit
			 * PLT entries sufficiently close to the branch if the
			 * section size exceeds the range of a branch
			 * instruction. So ignore relocations against defined
			 * symbols if they live in the same section as the
			 * relocation target.
			 */
			s = syms + ELF64_R_SYM(rela[i].r_info);
			if (s->st_shndx == dstidx)
				break;

			/*
			 * Jump relocations with non-zero addends against
			 * undefined symbols are supported by the ELF spec, but
			 * do not occur in practice (e.g., 'jump n bytes past
			 * the entry point of undefined function symbol f').
			 * So we need to support them, but there is no need to
			 * take them into consideration when trying to optimize
			 * this code. So let's only check for duplicates when
			 * the addend is zero: this allows us to record the PLT
			 * entry address in the symbol table itself, rather than
			 * having to search the list for duplicates each time we
			 * emit one.
			 */
			if (rela[i].r_addend != 0 || !duplicate_rel(rela, i))
				ret++;
			break;
		case R_AARCH64_ADR_PREL_PG_HI21_NC:
		case R_AARCH64_ADR_PREL_PG_HI21:
			if (!IS_ENABLED(CONFIG_ARM64_ERRATUM_843419) ||
			    !cpus_have_const_cap(ARM64_WORKAROUND_843419))
				break;

			/*
			 * Determine the minimal safe alignment for this ADRP
			 * instruction: the section alignment at which it is
			 * guaranteed not to appear at a vulnerable offset.
			 *
			 * This comes down to finding the least significant zero
			 * bit in bits [11:3] of the section offset, and
			 * increasing the section's alignment so that the
			 * resulting address of this instruction is guaranteed
			 * to equal the offset in that particular bit (as well
			 * as all less signficant bits). This ensures that the
			 * address modulo 4 KB != 0xfff8 or 0xfffc (which would
			 * have all ones in bits [11:3])
			 */
			min_align = 2ULL << ffz(rela[i].r_offset | 0x7);

			/*
			 * Allocate veneer space for each ADRP that may appear
			 * at a vulnerable offset nonetheless. At relocation
			 * time, some of these will remain unused since some
			 * ADRP instructions can be patched to ADR instructions
			 * instead.
			 */
			if (min_align > SZ_4K)
				ret++;
			else
				dstsec->sh_addralign = max(dstsec->sh_addralign,
							   min_align);
			break;
		}
	}
	return ret;
}

int module_frob_arch_sections(Elf_Ehdr *ehdr, Elf_Shdr *sechdrs,
			      char *secstrings, struct module *mod)
{
	unsigned long core_plts = 0;
	unsigned long init_plts = 0;
	Elf64_Sym *syms = NULL;
	Elf_Shdr *tramp = NULL;
	int i;

	/*
	 * Find the empty .plt section so we can expand it to store the PLT
	 * entries. Record the symtab address as well.
	 */
	for (i = 0; i < ehdr->e_shnum; i++) {
		if (!strcmp(secstrings + sechdrs[i].sh_name, ".plt"))
			mod->arch.core.plt = sechdrs + i;
		else if (!strcmp(secstrings + sechdrs[i].sh_name, ".init.plt"))
			mod->arch.init.plt = sechdrs + i;
		else if (IS_ENABLED(CONFIG_DYNAMIC_FTRACE) &&
			 !strcmp(secstrings + sechdrs[i].sh_name,
				 ".text.ftrace_trampoline"))
			tramp = sechdrs + i;
		else if (sechdrs[i].sh_type == SHT_SYMTAB)
			syms = (Elf64_Sym *)sechdrs[i].sh_addr;
	}

	if (!mod->arch.core.plt || !mod->arch.init.plt) {
		pr_err("%s: module PLT section(s) missing\n", mod->name);
		return -ENOEXEC;
	}
	if (!syms) {
		pr_err("%s: module symtab section missing\n", mod->name);
		return -ENOEXEC;
	}

	for (i = 0; i < ehdr->e_shnum; i++) {
		Elf64_Rela *rels = (void *)ehdr + sechdrs[i].sh_offset;
		int numrels = sechdrs[i].sh_size / sizeof(Elf64_Rela);
		Elf64_Shdr *dstsec = sechdrs + sechdrs[i].sh_info;

		if (sechdrs[i].sh_type != SHT_RELA)
			continue;

		/* ignore relocations that operate on non-exec sections */
		if (!(dstsec->sh_flags & SHF_EXECINSTR))
			continue;

		/* sort by type, symbol index and addend */
		sort(rels, numrels, sizeof(Elf64_Rela), cmp_rela, NULL);

		if (strncmp(secstrings + dstsec->sh_name, ".init", 5) != 0)
			core_plts += count_plts(syms, rels, numrels,
						sechdrs[i].sh_info, dstsec);
		else
			init_plts += count_plts(syms, rels, numrels,
						sechdrs[i].sh_info, dstsec);
	}

	mod->arch.core.plt->sh_type = SHT_NOBITS;
	mod->arch.core.plt->sh_flags = SHF_EXECINSTR | SHF_ALLOC;
	mod->arch.core.plt->sh_addralign = L1_CACHE_BYTES;
	mod->arch.core.plt->sh_size = (core_plts  + 1) * sizeof(struct plt_entry);
	mod->arch.core.plt_num_entries = 0;
	mod->arch.core.plt_max_entries = core_plts;

	mod->arch.init.plt->sh_type = SHT_NOBITS;
	mod->arch.init.plt->sh_flags = SHF_EXECINSTR | SHF_ALLOC;
	mod->arch.init.plt->sh_addralign = L1_CACHE_BYTES;
	mod->arch.init.plt->sh_size = (init_plts + 1) * sizeof(struct plt_entry);
	mod->arch.init.plt_num_entries = 0;
	mod->arch.init.plt_max_entries = init_plts;

	if (tramp) {
		tramp->sh_type = SHT_NOBITS;
		tramp->sh_flags = SHF_EXECINSTR | SHF_ALLOC;
		tramp->sh_addralign = __alignof__(struct plt_entry);
		tramp->sh_size = sizeof(struct plt_entry);
	}

	return 0;
}
