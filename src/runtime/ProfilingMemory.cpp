// MSVC binds a replacement of the global allocation functions per module, so
// the engine's own heap stays unreported.
#ifdef TRACY_ENABLE
#    include <tracy/Tracy.hpp>

#    include <cstddef>
#    include <cstdlib>
#    include <malloc.h>
#    include <new>

namespace
{
    void* Allocate(std::size_t a_size) noexcept
    {
        void* pointer = std::malloc(a_size == 0 ? 1 : a_size);
        if (pointer)
        {
            TracyAlloc(pointer, a_size);
        }
        return pointer;
    }

    void* AllocateAligned(std::size_t a_size, std::align_val_t a_alignment) noexcept
    {
        void* pointer = _aligned_malloc(a_size == 0 ? 1 : a_size, static_cast<std::size_t>(a_alignment));
        if (pointer)
        {
            TracyAlloc(pointer, a_size);
        }
        return pointer;
    }

    void Free(void* a_pointer) noexcept
    {
        if (a_pointer)
        {
            TracyFree(a_pointer);
            std::free(a_pointer);
        }
    }

    void FreeAligned(void* a_pointer) noexcept
    {
        if (a_pointer)
        {
            TracyFree(a_pointer);
            _aligned_free(a_pointer);
        }
    }

    void* AllocateOrThrow(std::size_t a_size)
    {
        if (void* pointer = Allocate(a_size))
        {
            return pointer;
        }
        throw std::bad_alloc{};
    }

    void* AllocateAlignedOrThrow(std::size_t a_size, std::align_val_t a_alignment)
    {
        if (void* pointer = AllocateAligned(a_size, a_alignment))
        {
            return pointer;
        }
        throw std::bad_alloc{};
    }
}

void* operator new(std::size_t a_size)
{
    return AllocateOrThrow(a_size);
}

void* operator new[](std::size_t a_size)
{
    return AllocateOrThrow(a_size);
}

void* operator new(std::size_t a_size, const std::nothrow_t&) noexcept
{
    return Allocate(a_size);
}

void* operator new[](std::size_t a_size, const std::nothrow_t&) noexcept
{
    return Allocate(a_size);
}

void* operator new(std::size_t a_size, std::align_val_t a_alignment)
{
    return AllocateAlignedOrThrow(a_size, a_alignment);
}

void* operator new[](std::size_t a_size, std::align_val_t a_alignment)
{
    return AllocateAlignedOrThrow(a_size, a_alignment);
}

void* operator new(std::size_t a_size, std::align_val_t a_alignment, const std::nothrow_t&) noexcept
{
    return AllocateAligned(a_size, a_alignment);
}

void* operator new[](std::size_t a_size, std::align_val_t a_alignment, const std::nothrow_t&) noexcept
{
    return AllocateAligned(a_size, a_alignment);
}

void operator delete(void* a_pointer) noexcept
{
    Free(a_pointer);
}

void operator delete[](void* a_pointer) noexcept
{
    Free(a_pointer);
}

void operator delete(void* a_pointer, std::size_t) noexcept
{
    Free(a_pointer);
}

void operator delete[](void* a_pointer, std::size_t) noexcept
{
    Free(a_pointer);
}

void operator delete(void* a_pointer, const std::nothrow_t&) noexcept
{
    Free(a_pointer);
}

void operator delete[](void* a_pointer, const std::nothrow_t&) noexcept
{
    Free(a_pointer);
}

void operator delete(void* a_pointer, std::align_val_t) noexcept
{
    FreeAligned(a_pointer);
}

void operator delete[](void* a_pointer, std::align_val_t) noexcept
{
    FreeAligned(a_pointer);
}

void operator delete(void* a_pointer, std::size_t, std::align_val_t) noexcept
{
    FreeAligned(a_pointer);
}

void operator delete[](void* a_pointer, std::size_t, std::align_val_t) noexcept
{
    FreeAligned(a_pointer);
}

void operator delete(void* a_pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    FreeAligned(a_pointer);
}

void operator delete[](void* a_pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    FreeAligned(a_pointer);
}
#endif
