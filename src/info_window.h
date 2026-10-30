// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// A small dark window for reference pages ("Controls and shortcuts", "About"):
// sections, key "chips" in a measured column so every description lines up
// exactly, word-wrapped paragraphs and bullet lists.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct InfoItem {
    enum class Kind { Header, Keys, Paragraph, Bullet, Gap, Subheader, Signature, Action };
    Kind kind = Kind::Paragraph;
    std::wstring keys;   // Keys: one chip per '|'-separated part, e.g. L"Ctrl|O" or L"\u2190|\u2192"
                         // Signature: the e-mail address (shown as a link)
    std::wstring text;
    std::function<void(HWND)> action;   // Action: called with this window when the link is clicked
};

struct InfoPage {
    std::wstring windowTitle;
    std::wstring title;
    std::wstring subtitle;
    std::vector<std::vector<InfoItem>> columns;   // shown side by side
    int columnWidth = 380;                        // at 100% scaling
};

// Helpers to build pages.
inline InfoItem InfoHeader(std::wstring t) { return {InfoItem::Kind::Header, {}, std::move(t), {}}; }
inline InfoItem InfoKeys(std::wstring k, std::wstring t) { return {InfoItem::Kind::Keys, std::move(k), std::move(t), {}}; }
inline InfoItem InfoText(std::wstring t) { return {InfoItem::Kind::Paragraph, {}, std::move(t), {}}; }
inline InfoItem InfoBullet(std::wstring t) { return {InfoItem::Kind::Bullet, {}, std::move(t), {}}; }
inline InfoItem InfoGap() { return {InfoItem::Kind::Gap, {}, {}, {}}; }
inline InfoItem InfoSubheader(std::wstring t) { return {InfoItem::Kind::Subheader, {}, std::move(t), {}}; }
// The author's logo (built into the .exe), "Made with <heart> from <place>" and an e-mail link.
inline InfoItem InfoSignature(std::wstring place, std::wstring email) {
    return {InfoItem::Kind::Signature, std::move(email), std::move(place), {}};
}

// A link-style line that runs `action` when clicked (e.g. opens another page).
inline InfoItem InfoAction(std::wstring t, std::function<void(HWND)> action) {
    InfoItem it{InfoItem::Kind::Action, {}, std::move(t), {}};
    it.action = std::move(action);
    return it;
}

// Shows the page centred over `owner` and returns when it is closed
// (Close button, Esc, Enter or the window's X).
void ShowInfoWindow(HWND owner, const InfoPage& page);
