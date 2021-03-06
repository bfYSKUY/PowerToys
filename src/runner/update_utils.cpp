#include "pch.h"

#include <common/common.h>
#include "Generated Files/resource.h"

#include "action_runner_utils.h"
#include "update_state.h"
#include "update_utils.h"

#include <common/timeutil.h>
#include <common/updating/installer.h>
#include <common/updating/updating.h>
#include <common/updating/notifications.h>
#include <runner/general_settings.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

auto Strings = create_notifications_strings();

bool start_msi_uninstallation_sequence()
{
    const auto package_path = updating::get_msi_package_path();

    if (package_path.empty())
    {
        // No MSI version detected
        return true;
    }

    if (!updating::offer_msi_uninstallation(Strings))
    {
        // User declined to uninstall or opted for "Don't show again"
        return false;
    }
    auto sei = launch_action_runner(L"-uninstall_msi");

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(sei.hProcess, &exit_code);
    CloseHandle(sei.hProcess);
    return exit_code == 0;
}

void github_update_worker()
{
    const int64_t update_check_period_minutes = 60 * 24;

    for (;;)
    {
        auto state = UpdateState::read();
        int64_t sleep_minutes_till_next_update = 0;
        if (state.github_update_last_checked_date.has_value())
        {
            int64_t last_checked_minutes_ago = timeutil::diff::in_minutes(timeutil::now(), *state.github_update_last_checked_date);
            if (last_checked_minutes_ago < 0)
            {
                last_checked_minutes_ago = update_check_period_minutes;
            }
            sleep_minutes_till_next_update = max(0, update_check_period_minutes - last_checked_minutes_ago);
        }

        std::this_thread::sleep_for(std::chrono::minutes(sleep_minutes_till_next_update));
        const bool download_updates_automatically = get_general_settings().downloadUpdatesAutomatically;
        try
        {
            updating::try_autoupdate(download_updates_automatically, Strings).get();
        }
        catch (...)
        {
            // Couldn't autoupdate
        }
        UpdateState::store([](UpdateState& state) {
            state.github_update_last_checked_date.emplace(timeutil::now());
        });
    }
}

std::optional<updating::new_version_download_info> check_for_updates()
{
    try
    {
        const auto new_version = updating::get_new_github_version_info_async(Strings).get();
        if (!new_version)
        {
            updating::notifications::show_unavailable(Strings, std::move(new_version.error()));
            return std::nullopt;
        }

        updating::notifications::show_available(new_version.value(), Strings);
        return std::move(new_version.value());
    }
    catch (...)
    {
        // Couldn't autoupdate
    }
    return std::nullopt;
}

bool launch_pending_update()
{
    try
    {
        auto update_state = UpdateState::read();
        if (update_state.pending_update)
        {
            UpdateState::store([](UpdateState& state) {
                state.pending_update = false;
                state.pending_installer_filename = {};
            });
            std::wstring args{ UPDATE_NOW_LAUNCH_STAGE1_START_PT_CMDARG };
            args += L' ';
            args += update_state.pending_installer_filename;

            launch_action_runner(args.c_str());
            return true;
        }
    }
    catch (...)
    {
    }
    return false;
}
