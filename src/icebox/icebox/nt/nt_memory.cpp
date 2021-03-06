#include "nt_os.hpp"

#define FDP_MODULE "nt::mem"
#include "endian.hpp"
#include "log.hpp"
#include "nt_mmu.hpp"

bool nt::Os::is_kernel_address(uint64_t ptr)
{
    return !!(ptr & 0xFFF0000000000000);
}

namespace
{
    struct ntphy_t
    {
        uint64_t ptr;
        bool     valid_page;
        bool     zero_page;
    };

    constexpr uint64_t mask(int bits)
    {
        return ~(~uint64_t(0) << bits);
    }

    ntphy_t physical_page(uint64_t pfn, uint64_t offset)
    {
        return ntphy_t{pfn + offset, true, false};
    }

    constexpr auto page_fault_required = ntphy_t{0, false, false};
    // constexpr auto zero_page        = ntphy_t{0, false, true};

    opt<MMPTE> read_pml4e(nt::Os& os, const virt_t& virt, dtb_t dtb)
    {
        const auto pml4e_base = dtb.val & (mask(40) << 12);
        const auto pml4e_ptr  = pml4e_base + virt.u.f.pml4 * 8;
        auto pml4e            = MMPTE{};
        const auto ok         = memory::read_physical(os.core_, &pml4e, pml4e_ptr, sizeof pml4e);
        if(!ok)
            return {};

        return pml4e;
    }

    opt<MMPTE> read_pdpe(nt::Os& os, const virt_t& virt, const MMPTE& pml4e)
    {
        auto pdpe           = MMPTE{};
        const auto pdpe_ptr = pml4e.u.hard.PageFrameNumber * PAGE_SIZE + virt.u.f.pdp * sizeof pdpe;
        auto ok             = memory::read_physical(os.core_, &pdpe, pdpe_ptr, sizeof pdpe);
        if(!ok)
            return {};

        return pdpe;
    }

    opt<MMPTE> read_pte(nt::Os& os, const virt_t& virt, const MMPTE& pde)
    {
        auto pte     = MMPTE{};
        auto pte_ptr = pde.u.hard.PageFrameNumber * PAGE_SIZE + virt.u.f.pt * sizeof pte;
        auto ok      = memory::read_physical(os.core_, &pte, pte_ptr, sizeof pte);
        if(!ok)
            return {};

        return pte;
    }

    opt<MMPTE> read_pde(nt::Os& os, const virt_t& virt, const MMPTE& pdpe)
    {
        auto pde           = MMPTE{};
        const auto pde_ptr = pdpe.u.hard.PageFrameNumber * PAGE_SIZE + virt.u.f.pd * sizeof pde;
        auto ok            = memory::read_physical(os.core_, &pde, pde_ptr, sizeof pde);
        if(!ok)
            return {};

        return pde;
    }

    opt<ntphy_t> virtual_to_physical(nt::Os& os, uint64_t ptr, proc_t* /*proc*/, dtb_t dtb)
    {
        const auto virt  = virt_t{read_le64(&ptr)};
        const auto pml4e = read_pml4e(os, virt, dtb);
        if(!pml4e)
            return {};

        if(!pml4e->u.hard.Valid)
            return page_fault_required;

        const auto pdpe = read_pdpe(os, virt, *pml4e);
        if(!pdpe)
            return {};

        if(!pdpe->u.hard.Valid)
            return page_fault_required;

        // 1g page
        if(pdpe->u.hard.LargePage)
        {
            const auto offset = ptr & mask(30);
            const auto base   = pdpe->u.value & (mask(22) << 30);
            return physical_page(base, offset);
        }

        const auto pde = read_pde(os, virt, *pdpe);
        if(!pde)
            return {};

        if(!pde->u.hard.Valid)
            return page_fault_required;

        // 2mb page
        if(pde->u.hard.LargePage)
        {
            const auto offset = ptr & mask(21);
            const auto base   = pde->u.value & (mask(31) << 21);
            return physical_page(base, offset);
        }

        const auto pte = read_pte(os, virt, *pde);
        if(!pte)
            return {};

        if(!pte->u.hard.Valid)
            return page_fault_required;

        return physical_page(pte->u.hard.PageFrameNumber * PAGE_SIZE, virt.u.f.offset);
    }

    enum class irql_e
    {
        passive  = 0,
        apc      = 1,
        dispatch = 2,
    };

    irql_e read_irql(core::Core& core)
    {
        return static_cast<irql_e>(registers::read(core, reg_e::cr8));
    }

    bool try_inject_page_fault(nt::Os& os, proc_t* proc, dtb_t dtb, uint64_t src)
    {
        // disable pf on kernel addresses
        if(os.is_kernel_address(src))
            return false;

        // disable pf without proc
        if(!proc)
            return false;

        // disable pf in IRQL >= dispatch
        const auto irql = read_irql(os.core_);
        if(irql >= irql_e::dispatch)
            return false;

        // disable pf on cr3 mismatch
        const auto cr3 = registers::read(os.core_, reg_e::cr3);
        if(proc && cr3 != proc->kdtb.val && cr3 != proc->udtb.val)
            return false;
        if(!proc && cr3 != dtb.val)
            return false;

        // check if input address is valid
        const auto opt_vma = os.vm_area_find(*proc, src);
        if(!opt_vma)
            return false;

        // check size
        const auto vma_span = os.vm_area_span(*proc, *opt_vma);
        if(!vma_span || src + PAGE_SIZE > vma_span->addr + vma_span->size)
            return false;

        // TODO missing vma access rights checks

        ++os.num_page_faults_;
        const auto cs       = registers::read(os.core_, reg_e::cs);
        const auto is_user  = nt::is_user_mode(cs);
        const auto code     = is_user ? 1 << 2 : 0;
        const auto injected = state::inject_interrupt(os.core_, PAGE_FAULT, code, src);
        if(!injected)
            return FAIL(false, "unable to inject page fault");

        state::run_to_current(os.core_, "inject_pf");
        return true;
    }
}

bool nt::Os::read_page(void* dst, uint64_t ptr, proc_t* proc, dtb_t dtb)
{
    const auto nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy)
        return false;

    if(nt_phy->zero_page)
    {
        memset(dst, 0, PAGE_SIZE);
        return true;
    }

    if(nt_phy->valid_page)
        return memory::read_physical(core_, dst, nt_phy->ptr, PAGE_SIZE);

    const auto ok = try_inject_page_fault(*this, proc, dtb, ptr);
    if(!ok)
        return false;

    return memory::read_virtual_with_dtb(core_, dtb, dst, ptr, PAGE_SIZE);
}

bool nt::Os::write_page(uint64_t ptr, const void* src, proc_t* proc, dtb_t dtb)
{
    const auto nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy)
        return false;

    if(nt_phy->valid_page)
        return memory::write_physical(core_, nt_phy->ptr, src, PAGE_SIZE);

    const auto ok = try_inject_page_fault(*this, proc, dtb, ptr);
    if(!ok)
        return false;

    return memory::write_virtual_with_dtb(core_, dtb, ptr, src, PAGE_SIZE);
}

opt<phy_t> nt::Os::virtual_to_physical(proc_t* proc, dtb_t dtb, uint64_t ptr)
{
    auto nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy)
        return {};

    if(nt_phy->valid_page)
        return phy_t{nt_phy->ptr};

    const auto ok = try_inject_page_fault(*this, proc, dtb, ptr);
    if(!ok)
        return {};

    nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy || !nt_phy->valid_page)
        return {};

    return phy_t{nt_phy->ptr};
}
