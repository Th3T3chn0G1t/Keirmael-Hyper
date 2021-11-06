#pragma once

#include "Common/Utilities.h"

// only placement new, no normal new/delete
inline void* operator new(size_t, void* ptr)
{
    return ptr;
}

inline void* operator new[](size_t, void* ptr)
{
    return ptr;
}

inline void operator delete(void*)
{
    panic("delete() called directly");
}

inline void operator delete(void* ptr, unsigned long)
{
    operator delete(ptr);
}

class MemoryServices;

namespace allocator {

// Sets new backend to use for allocations, returns the previous backend
// or nullptr if none was set.
MemoryServices* set_backend(MemoryServices*);

void* allocate_bytes(size_t);
void free_bytes(void*, size_t);

void* allocate_pages(size_t);
void free_pages(void*, size_t);

template <typename T, typename... Args>
T* allocate_new(Args&&... args)
{
    auto* data = allocate_bytes(sizeof(T));
    if (!data)
        return nullptr;

    return new (data) T(forward<Args>(args)...);
}

template <typename T>
T* allocate_new_array(size_t count)
{
    auto* data = allocate_bytes(count * sizeof(T));
    if (!data)
        return nullptr;

    return new (data) T[count] {};
}

template <typename T>
void free(T& object)
{
    object.~T();
    free_bytes(&object, sizeof(T));
}

template <typename T>
void free_array(T* array, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        array[i].~T();

    free_bytes(array, count * sizeof(T));
}

class ScopedPageAllocation {
public:
    ScopedPageAllocation(size_t count)
        : m_count(count)
    {
        m_address = allocate_pages(count);
    }

    [[nodiscard]] void* address() const { return m_address; }
    [[nodiscard]] size_t count() const { return m_count; }

    [[nodiscard]] bool failed() const { return m_address == nullptr; }

    template <typename T>
    T* as_pointer()
    {
        return reinterpret_cast<T*>(m_address);
    }

    ~ScopedPageAllocation()
    {
        if (m_address)
            free_pages(m_address, m_count);
    }

private:
    void* m_address { nullptr };
    size_t m_count { 0 };
};

}