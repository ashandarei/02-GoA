#include "kh02_index_state.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace kh02asset
{
    namespace
    {
        struct State
        {
            const AssetIndex empty;
            std::unique_ptr<const AssetIndex> configured;
            std::unique_ptr<const AssetIndex> indexed;
            std::atomic<const AssetIndex*> current{&empty};
            std::mutex publication_mutex;
        };

        auto state() -> State&
        {
            static State value;
            return value;
        }
    } // namespace

    auto asset_index() -> const AssetIndex&
    {
        return *state().current.load(std::memory_order_acquire);
    }

    void configure_source_identity(std::string build)
    {
        const auto tag = index_tag(build);
        auto& owner = state();
        std::lock_guard lock(owner.publication_mutex);
        if (owner.configured)
        {
            if (owner.configured->source_build != build)
            {
                throw std::runtime_error("kh02_source_identity_requires_restart");
            }
            return;
        }
        owner.configured =
            std::make_unique<const AssetIndex>(AssetIndex{tag, {}, {}, std::move(build)});
        owner.current.store(owner.configured.get(), std::memory_order_release);
    }

    void publish_index(AssetIndex index)
    {
        auto& owner = state();
        std::lock_guard lock(owner.publication_mutex);
        if (!owner.configured || index.source_build != owner.configured->source_build ||
            index.tag != owner.configured->tag || index.files.empty())
        {
            throw std::runtime_error("kh02_index_publication_identity");
        }
        if (owner.indexed)
        {
            if (serialize_index(*owner.indexed) != serialize_index(index))
            {
                throw std::runtime_error("kh02_source_index_requires_restart");
            }
            return;
        }
        owner.indexed = std::make_unique<const AssetIndex>(std::move(index));
        owner.current.store(owner.indexed.get(), std::memory_order_release);
    }

    auto index_ready() -> bool
    {
        return !asset_index().files.empty();
    }
} // namespace kh02asset
