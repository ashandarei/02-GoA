#include "kh3ap/core/uobject_validity.h"

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>

#include <cstddef>

#if defined(_MSC_VER)
#define NOMINMAX
#include <Windows.h>
#endif

namespace kh3ap
{
    namespace
    {

        auto is_uobject_slot_valid_unprotected(
                const RC::Unreal::UObject* object,
                int32_t object_count) -> bool
        {
            if (!object)
            {
                return false;
            }

            const int32_t object_index = object->GetInternalIndex();
            if (object_index < 0 || object_index >= object_count)
            {
                return false;
            }

            const auto* const object_item = object->GetObjectItem();
            return object_item &&
                    object_item->GetUObject() == object &&
                    RC::Unreal::UObjectArray::IsValid(
                            const_cast<RC::Unreal::FUObjectItem*>(object_item),
                            false) &&
                    !const_cast<RC::Unreal::UObject*>(object)->HasAnyFlags(
                            static_cast<RC::Unreal::EObjectFlags>(
                                    RC::Unreal::RF_BeginDestroyed |
                                    RC::Unreal::RF_FinishDestroyed));
        }

        auto is_uobject_valid_fast_unprotected(
                const RC::Unreal::UObject* object) -> bool
        {
            try
            {
                if (!object)
                {
                    return false;
                }
                const int32_t object_count = RC::Unreal::UObjectArray::GetNumElements();
                return is_uobject_slot_valid_unprotected(object, object_count);
            }
            catch (...)
            {
                return false;
            }
        }

#if defined(_MSC_VER)
        auto fast_uobject_validity_exception_filter(
                unsigned long exception_code) noexcept -> int
        {
            return exception_code == EXCEPTION_ACCESS_VIOLATION ||
                            exception_code == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH;
        }
#endif
    }

    auto is_uobject_valid_fast(
            const RC::Unreal::UObject* object) noexcept -> bool
    {
#if defined(_MSC_VER)
        __try
        {
            return is_uobject_valid_fast_unprotected(object);
        }
        __except (fast_uobject_validity_exception_filter(GetExceptionCode()))
        {
            return false;
        }
#else
        return is_uobject_valid_fast_unprotected(object);
#endif
    }

}
