// Copyright (C) 2022-2024 Vladislav Nepogodin
//
// This file is part of CachyOS kernel manager.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// NOLINTBEGIN(bugprone-unhandled-exception-at-new)

#include "schedext-window.hpp"
#include "utils.hpp"

#include <fstream>
#include <string>
#include <string_view>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsuggest-final-types"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <QProcess>
#include <QStringList>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <fmt/compile.h>
#include <fmt/core.h>

namespace {
auto read_kernel_file(std::string_view file_path) noexcept -> std::string {
    std::string file_content{};

    // Skip if failed to open file descriptor.
    std::ifstream file_stream{std::string{file_path}};
    if (!file_stream.is_open()) {
        return {};
    }
    if (!std::getline(file_stream, file_content)) {
        fmt::print(stderr, "Failed to read := '{}'\n", file_path);
        return {};
    }

    return file_content;
}

auto get_current_scheduler() noexcept -> std::string {
    using namespace std::string_view_literals;

    // NOTE: we assume that window won't be launched on kernel without sched_ext
    // e.g we won't show window at all in that case.
    const auto& current_state = read_kernel_file("/sys/kernel/sched_ext/state"sv);
    if (current_state != "enabled"sv) {
        return current_state;
    }
    const auto& current_sched = read_kernel_file("/sys/kernel/sched_ext/root/ops"sv);
    if (current_sched.empty()) {
        return std::string{"unknown"sv};
    }
    return current_sched;
}

auto is_scx_service_enabled() noexcept -> bool {
    using namespace std::string_view_literals;
    return utils::exec("systemctl is-enabled scx"sv) == "enabled"sv;
}

auto is_scx_service_active() noexcept -> bool {
    using namespace std::string_view_literals;
    return utils::exec("systemctl is-active scx"sv) == "active"sv;
}
}  // namespace

SchedExtWindow::SchedExtWindow(QWidget* parent)
  : QMainWindow(parent), m_sched_timer(new QTimer(this)) {
    m_ui->setupUi(this);

    setAttribute(Qt::WA_NativeWindow);
    setWindowFlags(Qt::Window);  // for the close, min and max buttons

    // Selecting the scheduler
    QStringList sched_names;
    sched_names << "scx_bpfland"
                << "scx_central"
                << "scx_lavd"
                << "scx_layered"
                << "scx_nest"
                << "scx_qmap"
                << "scx_rlfifo"
                << "scx_rustland"
                << "scx_rusty"
                << "scx_simple"
                << "scx_userland";
    m_ui->schedext_combo_box->addItems(sched_names);

    m_ui->current_sched_label->setText(QString::fromStdString(get_current_scheduler()));

    // setup timer
    using namespace std::chrono_literals;  // NOLINT

    connect(m_sched_timer, &QTimer::timeout, this, &SchedExtWindow::update_current_sched);
    m_sched_timer->start(1s);

    // Connect buttons signal
    connect(m_ui->apply_button, &QPushButton::clicked, this, &SchedExtWindow::on_apply);
    connect(m_ui->disable_button, &QPushButton::clicked, this, &SchedExtWindow::on_disable);
}

void SchedExtWindow::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
}

void SchedExtWindow::update_current_sched() noexcept {
    m_ui->current_sched_label->setText(QString::fromStdString(get_current_scheduler()));
}

void SchedExtWindow::on_disable() noexcept {
    m_ui->disable_button->setEnabled(false);
    m_ui->apply_button->setEnabled(false);

    using namespace std::string_view_literals;
    // TODO(vnepogodin): refactor that
    if (is_scx_service_enabled()) {
        QProcess::startDetached("/usr/bin/pkexec", {"/usr/bin/systemctl", "disable", "--now", "scx"});
        fmt::print("Disabling scx\n");
    } else if (is_scx_service_active()) {
        QProcess::startDetached("/usr/bin/pkexec", {"/usr/bin/systemctl", "stop", "scx"});
        fmt::print("Stoping scx\n");
    }

    m_ui->disable_button->setEnabled(true);
    m_ui->apply_button->setEnabled(true);
}

void SchedExtWindow::on_apply() noexcept {
    m_ui->disable_button->setEnabled(false);
    m_ui->apply_button->setEnabled(false);

    const auto service_cmd = []() -> std::string_view {
        using namespace std::string_view_literals;
        if (!is_scx_service_enabled()) {
            return "enable --now"sv;
        }
        return "restart"sv;
    }();

    static constexpr auto is_flags_commented = []() -> bool {
        using namespace std::string_view_literals;
        static constexpr auto scx_conf_path = "/etc/default/scx"sv;
        const auto& scx_conf_content        = utils::read_whole_file(scx_conf_path);
        return scx_conf_content.find("#SCX_FLAGS="sv) != std::string::npos;
    };
    static constexpr auto get_scx_flags_sed = [](std::string_view sched_flags_text, bool flags_commented) -> std::string {
        using namespace std::string_literals;
        if (sched_flags_text.empty() && !flags_commented) {
            // comment out flags in scx
            return "-e 's/SCX_FLAGS=/#SCX_FLAGS=/'"s;
        } else if (!sched_flags_text.empty() && flags_commented) {
            // set flags in scx
            return fmt::format(R"(-e "s/.*SCX_FLAGS=.*/SCX_FLAGS='{}'/")", sched_flags_text);
        } else if (!sched_flags_text.empty() && !flags_commented) {
            // set flags in scx
            return fmt::format(R"(-e "s/SCX_FLAGS=.*/SCX_FLAGS='{}'/")", sched_flags_text);
        }
        return ""s;
    };

    const bool flags_commented = is_flags_commented();

    // TODO(vnepogodin): refactor that
    const auto& sched_flags_text = m_ui->schedext_flags_edit->text().trimmed().toStdString();
    const auto& scx_flags_sed    = get_scx_flags_sed(sched_flags_text, flags_commented);

    const auto& current_selected = m_ui->schedext_combo_box->currentText().toStdString();
    const auto& sed_cmd          = fmt::format("sed -e 's/SCX_SCHEDULER=.*/SCX_SCHEDULER={}/' {} -i /etc/default/scx && systemctl {} scx", current_selected, scx_flags_sed, service_cmd);
    QProcess::startDetached("/usr/bin/pkexec", {"/usr/bin/bash", "-c", QString::fromStdString(sed_cmd)});
    fmt::print("Applying scx {}\n", current_selected);

    m_ui->disable_button->setEnabled(true);
    m_ui->apply_button->setEnabled(true);
}

// NOLINTEND(bugprone-unhandled-exception-at-new)
