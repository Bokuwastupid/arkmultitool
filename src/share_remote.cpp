#include "kopt/share_remote.hpp"

#include <utility>

namespace kopt::share
{
    ReporterFilter::ReporterFilter(const std::uint64_t own_stable_id, const float radius_cm) noexcept
        : own_stable_id_(own_stable_id), radius_cm_(radius_cm)
    {
    }

    bool ReporterFilter::accept(const std::uint64_t reporter_stable_id,
        const Vec3& reporter_position, const Vec3& my_position) const noexcept
    {
        if (reporter_stable_id == own_stable_id_) return false;
        const float dx = reporter_position.x - my_position.x;
        const float dy = reporter_position.y - my_position.y;
        const float dz = reporter_position.z - my_position.z;
        const float distance_sq = dx * dx + dy * dy + dz * dz;
        return distance_sq > radius_cm_ * radius_cm_;
    }

    RemoteView::RemoteView(const std::chrono::milliseconds ttl) noexcept : ttl_(ttl) {}

    void RemoteView::update(RemoteBatch batch)
    {
        const std::string key = batch.reporter_account_id;
        by_reporter_[key] = std::move(batch);
    }

    std::vector<RemoteBatch> RemoteView::visible(const std::chrono::steady_clock::time_point now)
    {
        std::vector<RemoteBatch> out;
        out.reserve(by_reporter_.size());
        for (auto it = by_reporter_.begin(); it != by_reporter_.end();)
        {
            if (now - it->second.received_at > ttl_)
            {
                it = by_reporter_.erase(it);
                continue;
            }
            out.push_back(it->second);
            ++it;
        }
        return out;
    }
}
