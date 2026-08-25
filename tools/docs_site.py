#!/usr/bin/env python3
"""uniOS wiki generator.

Renders docs/reference/**/*.md into a static, dependency-free documentation
site that matches the landing page design language. Python stdlib only.

Features:
  - Markdown subset: headings, paragraphs, fenced code, inline code, bold,
    italic, links, images, nested lists, GFM tables, blockquotes, hr.
  - Strict mode (default): broken internal links, unknown anchors, unlisted
    pages, and missing images fail the build.
  - Client-side search over a generated JSON index.
  - Sidebar navigation, per-page TOC, prev/next pager, dark/light theme.

Usage:
  python3 tools/docs_site.py --source docs/reference --dest build/wiki
"""

import argparse
import html
import json
import os
import re
import sys

GITHUB_TREE_DOCS = "https://github.com/unionyxx/uniOS/tree/main/docs/reference"
GITHUB_REPO = "https://github.com/unionyxx/uniOS"

NAV = [
    ("Overview", [
        ("Home", "index.md", False),
        ("Architecture", "architecture.md", False),
    ]),
    ("Getting Started", [
        ("Building and Running", "build.md", False),
        ("Boot Images", "images.md", False),
    ]),
    ("Kernel", [
        ("Boot Path", "boot.md", False),
        ("Memory Management", "memory.md", False),
        ("Scheduling", "scheduling.md", False),
        ("SMP", "smp.md", False),
        ("Processes", "processes.md", False),
        ("System Calls", "syscalls.md", False),
        ("VFS", "vfs.md", False),
        ("Filesystems", "filesystems.md", False),
        ("Drivers", "drivers.md", False),
        ("Storage Drivers", "storage-drivers.md", False),
        ("USB", "usb.md", False),
        ("Input", "input.md", False),
        ("Display", "display.md", False),
        ("Audio", "audio.md", False),
        ("Networking", "networking.md", False),
        ("TCP", "tcp.md", False),
    ]),
    ("Userspace", [
        ("Userspace Runtime", "userspace.md", False),
        ("Window Manager", "wm.md", False),
        ("Shell", "shell.md", False),
        ("Shell Scripting", "scripting.md", False),
        ("Desktop and Apps", "apps.md", False),
    ]),
    ("Reference", [
        ("Runtime Configuration", "config.md", False),
        ("Testing", "testing.md", False),
        ("Asset Formats", "formats/README.md", False),
        ("UOIC Icons", "formats/uoic.md", True),
        ("UOCU Cursors", "formats/uocu.md", True),
        ("UOF Fonts", "formats/uof.md", True),
        ("UOWP Wallpapers", "formats/uowp.md", True),
    ]),
]

CSS = """\
:root {
    --bg: #ffffff;
    --text: #1d1d1f;
    --muted: #86868b;
    --link: #0066cc;
    --border: #d2d2d7;
    --nav-bg: rgba(255, 255, 255, 0.72);
    --surface: #f5f5f7;
    --surface-border: #e5e5ea;
    --radius-sm: 8px;
    --radius-md: 16px;
    --radius-pill: 9999px;
    --ease: cubic-bezier(0.4, 0, 0.2, 1);
    --dur: 0.4s;
    --mono: ui-monospace, "SF Mono", SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
}

[data-theme="dark"] {
    --bg: #000000;
    --text: #f5f5f7;
    --muted: #a1a1a6;
    --border: #333336;
    --nav-bg: rgba(0, 0, 0, 0.72);
    --surface: #111111;
    --surface-border: #222222;
}

* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
    scrollbar-width: thin;
    scrollbar-color: var(--border) transparent;
}

*::-webkit-scrollbar {
    width: 8px;
    height: 8px;
}

*::-webkit-scrollbar-track {
    background: transparent;
}

*::-webkit-scrollbar-thumb {
    background: var(--border);
    border-radius: var(--radius-pill);
}

*::-webkit-scrollbar-thumb:hover {
    background: var(--muted);
}

html {
    scroll-behavior: smooth;
}

body {
    font-family: "Inter", -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background-color: var(--bg);
    color: var(--text);
    line-height: 1.5;
    -webkit-font-smoothing: antialiased;
    -moz-osx-font-smoothing: grayscale;
    letter-spacing: -0.015em;
    transition: background-color var(--dur) var(--ease), color var(--dur) var(--ease);
}

:focus-visible {
    outline: 2px solid var(--link);
    outline-offset: 2px;
    border-radius: 4px;
}

h1, h2, h3 {
    letter-spacing: -0.02em;
}

nav {
    position: sticky;
    top: 0;
    z-index: 100;
    background: var(--nav-bg);
    backdrop-filter: saturate(180%) blur(20px);
    -webkit-backdrop-filter: saturate(180%) blur(20px);
    border-bottom: 1px solid rgba(134, 134, 139, 0.15);
    transition: background-color var(--dur) var(--ease);
}

.nav-content {
    max-width: 1200px;
    margin: 0 auto;
    padding: 1rem 2rem;
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 1rem;
}

.logo {
    font-weight: 600;
    font-size: 1.1rem;
    color: var(--text);
    text-decoration: none;
}

.nav-links {
    display: flex;
    gap: 2rem;
    align-items: center;
}

.nav-links a {
    color: var(--text);
    text-decoration: none;
    font-size: 0.85rem;
    font-weight: 500;
    transition: color 0.2s var(--ease);
}

.nav-links a:hover {
    color: var(--muted);
}

.nav-links a.active {
    color: var(--muted);
}

.nav-github {
    display: flex;
    align-items: center;
    gap: 0.5rem;
}

.menu-btn {
    display: none;
    border: none;
    background: transparent;
    color: var(--text);
    cursor: pointer;
    padding: 0.25rem;
}

.theme-switch-mini {
    display: flex;
    position: relative;
    background: var(--surface);
    border: 1px solid var(--surface-border);
    padding: 3px;
    border-radius: var(--radius-pill);
    transition: background-color var(--dur) var(--ease), border-color var(--dur) var(--ease);
}

.theme-switch-mini button {
    position: relative;
    z-index: 2;
    padding: 0.15rem 0.85rem;
    border-radius: var(--radius-pill);
    font-size: 0.7rem;
    line-height: 1;
    font-weight: 500;
    font-family: inherit;
    color: var(--muted);
    cursor: pointer;
    border: none;
    background: transparent;
    transition: color 0.3s var(--ease);
}

.theme-switch-mini button.active {
    color: var(--text);
}

.theme-switch-mini::before {
    content: '';
    position: absolute;
    top: 3px;
    left: 3px;
    width: calc(50% - 3px);
    height: calc(100% - 6px);
    background: var(--bg);
    border-radius: var(--radius-pill);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
    z-index: 1;
    transition: transform 0.3s var(--ease), background-color var(--dur) var(--ease);
}

[data-theme="light"] .theme-switch-mini::before {
    transform: translateX(100%);
}

.docs-layout {
    display: grid;
    grid-template-columns: 264px minmax(0, 1fr);
    gap: 0 3rem;
    max-width: 1200px;
    margin: 0 auto;
    padding: 2.5rem 2rem 4rem;
    align-items: start;
}

.docs-sidebar {
    position: sticky;
    top: 70px;
    max-height: calc(100vh - 90px);
    overflow-y: auto;
    padding-bottom: 1rem;
    padding-right: 0.6rem;
}

.search {
    position: relative;
    margin-bottom: 1.5rem;
}

.search input {
    width: 100%;
    padding: 0.55rem 0.9rem;
    border-radius: var(--radius-sm);
    border: 1px solid var(--surface-border);
    background: var(--surface);
    color: var(--text);
    font-family: inherit;
    font-size: 0.85rem;
    transition: border-color 0.2s var(--ease), background-color var(--dur) var(--ease);
}

.search input::placeholder {
    color: var(--muted);
}

.search input:focus {
    outline: none;
    border-color: var(--border);
}

.search-results {
    position: absolute;
    top: calc(100% + 6px);
    left: 0;
    right: 0;
    background: var(--bg);
    border: 1px solid var(--surface-border);
    border-radius: var(--radius-sm);
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.14);
    z-index: 60;
    max-height: 340px;
    overflow-y: auto;
    display: none;
}

.search-results.open {
    display: block;
}

.search-result {
    display: block;
    padding: 0.6rem 0.9rem;
    text-decoration: none;
    color: var(--text);
    border-bottom: 1px solid var(--surface-border);
    font-size: 0.85rem;
}

.search-result:last-child {
    border-bottom: none;
}

.search-result:hover {
    background: var(--surface);
}

.sr-title {
    font-weight: 600;
    display: flex;
    justify-content: space-between;
    gap: 0.75rem;
}

.sr-section {
    color: var(--muted);
    font-size: 0.72rem;
    font-weight: 400;
    white-space: nowrap;
}

.sr-snippet {
    color: var(--muted);
    font-size: 0.78rem;
    margin-top: 2px;
    line-height: 1.45;
}

.search-empty {
    padding: 0.75rem 0.9rem;
    color: var(--muted);
    font-size: 0.82rem;
}

mark {
    background: rgba(0, 102, 204, 0.16);
    color: inherit;
    border-radius: 2px;
    padding: 0 1px;
}

.nav-section {
    margin: 1.5rem 0 0.5rem;
    font-size: 0.7rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.1em;
    color: var(--muted);
}

.nav-section:first-of-type {
    margin-top: 0;
}

.nav-item {
    display: block;
    padding: 0.32rem 0.6rem;
    margin: 1px 0;
    border-radius: 6px;
    color: var(--muted);
    text-decoration: none;
    font-size: 0.875rem;
    transition: color 0.2s var(--ease), background-color 0.2s var(--ease);
}

.nav-item:hover {
    color: var(--text);
    background: var(--surface);
}

.nav-item.active {
    color: var(--text);
    font-weight: 500;
    background: var(--surface);
}

.nav-item.sub {
    padding-left: 1.5rem;
    font-size: 0.84rem;
}

.docs-main {
    min-width: 0;
}

.docs-main.has-toc {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 200px;
    gap: 2.5rem;
    align-items: start;
}

article {
    max-width: 760px;
    min-width: 0;
}

article h1 {
    font-size: 2.1rem;
    font-weight: 700;
    line-height: 1.15;
    margin: 0.25rem 0 1.25rem;
}

article h2 {
    font-size: 1.4rem;
    font-weight: 600;
    margin: 2.5rem 0 0.9rem;
    scroll-margin-top: 76px;
}

article h3 {
    font-size: 1.12rem;
    font-weight: 600;
    margin: 1.9rem 0 0.7rem;
    scroll-margin-top: 76px;
}

article h4, article h5, article h6 {
    font-size: 0.98rem;
    font-weight: 600;
    margin: 1.5rem 0 0.5rem;
    scroll-margin-top: 76px;
}

article p {
    margin: 0 0 1rem;
    line-height: 1.65;
    font-size: 0.95rem;
}

article ul, article ol {
    margin: 0 0 1rem;
    padding-left: 1.4rem;
}

article li {
    margin: 0.3rem 0;
    line-height: 1.6;
    font-size: 0.95rem;
}

article li > ul, article li > ol {
    margin: 0.25rem 0 0.25rem;
}

article a {
    color: var(--link);
    text-decoration: none;
}

article a:hover {
    text-decoration: underline;
}

article code {
    font-family: var(--mono);
    font-size: 0.8em;
    background: var(--surface);
    border: 1px solid var(--surface-border);
    border-radius: 5px;
    padding: 0.1em 0.35em;
    white-space: nowrap;
}

article pre {
    background: var(--surface);
    border: 1px solid var(--surface-border);
    border-radius: var(--radius-sm);
    padding: 0.9rem 1.1rem;
    overflow-x: auto;
    margin: 0 0 1.25rem;
}

article pre code {
    background: none;
    border: none;
    padding: 0;
    font-size: 0.8rem;
    line-height: 1.6;
    white-space: pre;
}

.table-wrap {
    overflow-x: auto;
    margin: 0 0 1.25rem;
}

article table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.875rem;
}

article th {
    text-align: left;
    font-weight: 600;
    padding: 0.5rem 0.75rem;
    border-bottom: 1px solid var(--border);
    white-space: nowrap;
}

article td {
    padding: 0.5rem 0.75rem;
    border-bottom: 1px solid var(--surface-border);
    vertical-align: top;
    line-height: 1.55;
}

article blockquote {
    border-left: 3px solid var(--link);
    background: var(--surface);
    border-radius: 0 var(--radius-sm) var(--radius-sm) 0;
    padding: 0.7rem 1rem;
    margin: 0 0 1.25rem;
    font-size: 0.9rem;
}

article blockquote p {
    margin: 0;
}

article hr {
    border: none;
    border-top: 1px solid var(--surface-border);
    margin: 2rem 0;
}

article img {
    max-width: 100%;
    border-radius: var(--radius-sm);
}

.toc {
    position: sticky;
    top: 70px;
    max-height: calc(100vh - 90px);
    overflow-y: auto;
    font-size: 0.8rem;
}

.toc-title {
    font-size: 0.7rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.1em;
    color: var(--muted);
    margin-bottom: 0.6rem;
}

.toc a {
    display: block;
    color: var(--muted);
    text-decoration: none;
    padding: 0.28rem 0 0.28rem 0.75rem;
    border-left: 1px solid var(--surface-border);
    line-height: 1.4;
    transition: color 0.2s var(--ease);
}

.toc a:hover {
    color: var(--text);
}

.toc a.l3 {
    padding-left: 1.5rem;
}

.pager {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 1rem;
    max-width: 760px;
    margin-top: 3.5rem;
    padding-top: 1.5rem;
    border-top: 1px solid var(--surface-border);
}

.pager a {
    display: flex;
    flex-direction: column;
    min-width: 0;
    border: 1px solid var(--surface-border);
    border-radius: var(--radius-sm);
    padding: 0.9rem 1.1rem;
    text-decoration: none;
    color: var(--text);
    font-size: 0.9rem;
    transition: border-color 0.2s var(--ease), background-color 0.2s var(--ease);
}

.pager a:hover {
    border-color: var(--border);
    background: var(--surface);
}

.pager .label {
    display: flex;
    align-items: center;
    gap: 0.35rem;
    font-size: 0.68rem;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--muted);
    margin-bottom: 0.4rem;
}

.pager .chevron {
    font-size: 0.9rem;
    line-height: 1;
    transition: transform 0.2s var(--ease);
}

.pager a.prev:hover .chevron {
    transform: translateX(-3px);
}

.pager a.next:hover .chevron {
    transform: translateX(3px);
}

.pager .title {
    display: block;
    font-weight: 500;
    line-height: 1.4;
    overflow-wrap: anywhere;
}

.pager .next {
    text-align: right;
    align-items: flex-end;
}

.docs-footer-wrap {
    max-width: 1200px;
    margin: 0 auto;
    padding: 0 2rem 3rem;
}

.docs-footer {
    padding: 2rem 0 0;
    border-top: 1px solid var(--border);
    display: flex;
    justify-content: space-between;
    align-items: center;
    color: var(--muted);
    font-size: 0.85rem;
    transition: border-color var(--dur) var(--ease);
}

.docs-footer .footer-logo {
    font-weight: 600;
    color: var(--text);
}

.sidebar-backdrop {
    display: none;
}

@media (max-width: 1100px) {
    .docs-main.has-toc {
        grid-template-columns: minmax(0, 1fr);
    }

    .toc {
        display: none;
    }
}

@media (max-width: 900px) {
    .docs-layout {
        grid-template-columns: 1fr;
        padding: 1.75rem 1.25rem 3rem;
    }

    .menu-btn {
        display: inline-flex;
        align-items: center;
    }

    .nav-content {
        padding: 1rem 1.25rem;
    }

    .nav-links {
        gap: 1.25rem;
    }

    .nav-links a:not(.active) {
        display: none;
    }

    .docs-sidebar {
        position: fixed;
        top: 0;
        left: 0;
        bottom: 0;
        width: 292px;
        max-height: none;
        background: var(--bg);
        border-right: 1px solid var(--surface-border);
        padding: 1.5rem 1.25rem;
        z-index: 200;
        transform: translateX(-105%);
        transition: transform 0.3s var(--ease);
    }

    body.sidebar-open .docs-sidebar {
        transform: none;
    }

    .sidebar-backdrop {
        position: fixed;
        inset: 0;
        background: rgba(0, 0, 0, 0.4);
        z-index: 150;
    }

    body.sidebar-open {
        overflow: hidden;
    }

    body.sidebar-open .sidebar-backdrop {
        display: block;
    }

    .pager {
        grid-template-columns: 1fr;
    }

    .docs-footer-wrap {
        padding: 0 1.25rem 2rem;
    }
}
"""

JS = """\
(function () {
    'use strict';

    var root = document.documentElement;

    function updateThemeButtons(theme) {
        var buttons = document.querySelectorAll('[data-theme-btn]');
        for (var i = 0; i < buttons.length; i++) {
            buttons[i].classList.toggle('active', buttons[i].getAttribute('data-theme-btn') === theme);
        }
    }

    function setTheme(theme) {
        root.setAttribute('data-theme', theme);
        try { localStorage.setItem('theme', theme); } catch (e) {}
        updateThemeButtons(theme);
    }

    var buttons = document.querySelectorAll('[data-theme-btn]');
    for (var i = 0; i < buttons.length; i++) {
        buttons[i].addEventListener('click', function () {
            setTheme(this.getAttribute('data-theme-btn'));
        });
    }

    var initial = null;
    try { initial = localStorage.getItem('theme'); } catch (e) {}
    if (initial !== 'light' && initial !== 'dark') {
        initial = (window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches) ? 'dark' : 'light';
    }
    setTheme(initial);

    var menuBtn = document.getElementById('menu-btn');
    var backdrop = document.getElementById('sidebar-backdrop');

    function closeSidebar() { document.body.classList.remove('sidebar-open'); }

    if (menuBtn) {
        menuBtn.addEventListener('click', function () {
            document.body.classList.toggle('sidebar-open');
        });
    }
    if (backdrop) { backdrop.addEventListener('click', closeSidebar); }
    var navItems = document.querySelectorAll('.nav-item');
    for (var n = 0; n < navItems.length; n++) {
        navItems[n].addEventListener('click', closeSidebar);
    }

    var input = document.getElementById('doc-search');
    var panel = document.getElementById('search-results');
    var pages = null;

    function escapeHtml(text) {
        return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    }

    function highlight(text, terms) {
        var escaped = escapeHtml(text);
        for (var i = 0; i < terms.length; i++) {
            var re = new RegExp('(' + terms[i].replace(/[.*+?^${}()|[\\]\\\\]/g, '\\\\$&') + ')', 'gi');
            escaped = escaped.replace(re, '<mark>$1</mark>');
        }
        return escaped;
    }

    function snippetFor(text, terms) {
        var lower = text.toLowerCase();
        var at = -1;
        for (var i = 0; i < terms.length; i++) {
            at = lower.indexOf(terms[i]);
            if (at >= 0) break;
        }
        if (at < 0) at = 0;
        var start = Math.max(0, at - 45);
        var end = Math.min(text.length, start + 160);
        var snippet = text.slice(start, end);
        if (start > 0) snippet = '…' + snippet;
        if (end < text.length) snippet = snippet + '…';
        return snippet;
    }

    function runSearch(query) {
        if (!pages || !panel) return;
        var terms = query.toLowerCase().split(/[^a-z0-9_]+/).filter(function (t) { return t.length >= 2; });
        if (terms.length === 0) {
            panel.classList.remove('open');
            panel.innerHTML = '';
            return;
        }
        var scored = [];
        for (var i = 0; i < pages.length; i++) {
            var page = pages[i];
            var title = page.t.toLowerCase();
            var body = page.b.toLowerCase();
            var score = 0;
            var matched = 0;
            for (var j = 0; j < terms.length; j++) {
                var inTitle = title.indexOf(terms[j]) >= 0;
                var bodyAt = body.indexOf(terms[j]);
                if (inTitle) score += 120;
                if (bodyAt >= 0) {
                    matched++;
                    var count = 0;
                    var pos = 0;
                    while ((pos = body.indexOf(terms[j], pos)) >= 0 && count < 8) { count++; pos += terms[j].length; }
                    score += count * 6;
                }
            }
            if (matched < terms.length && score < 120 * terms.length) {
                if (matched === 0) score = 0;
            }
            if (matched === 0) continue;
            if (matched < terms.length) score = Math.floor(score / 2);
            scored.push({ page: page, score: score });
        }
        scored.sort(function (a, b) { return b.score - a.score; });
        scored = scored.slice(0, 10);
        if (scored.length === 0) {
            panel.innerHTML = '<div class="search-empty">No results for “' + escapeHtml(query) + '”</div>';
            panel.classList.add('open');
            return;
        }
        var out = '';
        for (var k = 0; k < scored.length; k++) {
            var p = scored[k].page;
            out += '<a class="search-result" href="' + input.dataset.base + p.u + '">'
                + '<span class="sr-title"><span>' + highlight(p.t, terms) + '</span>'
                + '<span class="sr-section">' + escapeHtml(p.s) + '</span></span>'
                + '<span class="sr-snippet">' + highlight(snippetFor(p.b, terms), terms) + '</span>'
                + '</a>';
        }
        panel.innerHTML = out;
        panel.classList.add('open');
    }

    if (input && panel) {
        var timer = null;
        input.addEventListener('input', function () {
            if (timer) clearTimeout(timer);
            var value = input.value;
            timer = setTimeout(function () { runSearch(value); }, 90);
        });
        input.addEventListener('focus', function () {
            if (input.value.trim().length >= 2) runSearch(input.value);
        });
        input.addEventListener('keydown', function (e) {
            if (e.key === 'Escape') {
                panel.classList.remove('open');
                input.blur();
            }
        });
        document.addEventListener('click', function (e) {
            if (!panel.contains(e.target) && e.target !== input) {
                panel.classList.remove('open');
            }
        });
        document.addEventListener('keydown', function (e) {
            if (e.key === '/' && document.activeElement !== input
                && !/^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName)) {
                e.preventDefault();
                input.focus();
            }
        });
        fetch(input.dataset.base + 'search_index.json')
            .then(function (r) { return r.json(); })
            .then(function (data) { pages = data; })
            .catch(function () { pages = null; });
    }
})();
"""

PAGE_TEMPLATE = """<!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>{title} · uniOS</title>
    <meta name="description" content="{description}">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
    <link rel="stylesheet" href="{wbase}wiki.css">
    <link rel="icon" href="{base}assets/site/favicon.ico">
    <script>
        (function () {{
            var saved = null;
            try {{ saved = localStorage.getItem('theme'); }} catch (e) {{}}
            var dark = saved ? saved === 'dark'
                : window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
            document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
        }})();
    </script>
</head>

<body>
    <nav>
        <div class="nav-content">
            <a class="logo" href="{base}index.html">uniOS</a>
            <div class="nav-links">
                <a href="{github_docs}">Docs</a>
                <a href="{base}wiki/index.html" class="active">Wiki</a>
                <div class="theme-switch-mini">
                    <button type="button" data-theme-btn="dark">Dark</button>
                    <button type="button" data-theme-btn="light">Light</button>
                </div>
                <a href="{github_repo}" class="nav-github">
                    <svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
                        <path
                            d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z" />
                    </svg>
                    GitHub
                </a>
                <button class="menu-btn" id="menu-btn" type="button" aria-label="Toggle navigation">
                    <svg width="18" height="18" viewBox="0 0 18 18" fill="none" aria-hidden="true">
                        <path d="M2 4.5h14M2 9h14M2 13.5h14" stroke="currentColor" stroke-width="1.5"
                            stroke-linecap="round" />
                    </svg>
                </button>
            </div>
        </div>
    </nav>
    <div class="docs-layout">
        <aside class="docs-sidebar">
            <div class="search">
                <input id="doc-search" type="search" placeholder="Search docs…  ( / )" autocomplete="off"
                    spellcheck="false" data-base="{wbase}">
                <div class="search-results" id="search-results"></div>
            </div>
            {sidebar}
        </aside>
        <div class="sidebar-backdrop" id="sidebar-backdrop"></div>
        <main class="docs-main{toc_class}">
            <article>
{article}
            </article>
            {toc}
            {pager}
        </main>
    </div>
    <div class="docs-footer-wrap">
        <footer class="docs-footer">
            <div class="footer-logo">uniOS</div>
            <div>MIT License &bull; 2026</div>
        </footer>
    </div>
    <script src="{wbase}wiki.js"></script>
</body>

</html>
"""


class Page:
    def __init__(self, rel_md, nav_title, section):
        self.rel_md = rel_md
        self.rel_html = re.sub(r"README\.md$", "index.html", rel_md)
        self.rel_html = re.sub(r"\.md$", ".html", self.rel_html)
        self.nav_title = nav_title
        self.section = section
        self.title = nav_title
        self.html = ""
        self.toc = []
        self.text = ""
        self.description = ""
        self.links = []


def slugify(text, seen):
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    if not slug:
        slug = "section"
    base = slug
    i = 1
    while slug in seen:
        i += 1
        slug = "%s-%d" % (base, i)
    seen.add(slug)
    return slug


class MarkdownError(Exception):
    pass


class MarkdownRenderer:
    def __init__(self, page, source_root, errors):
        self.page = page
        self.source_root = source_root
        self.errors = errors
        self.seen_ids = set()

    def add_error(self, message):
        self.errors.append("%s: %s" % (self.page.rel_md, message))

    def resolve_link(self, target):
        """Rewrite an internal link target to its output .html path.

        Returns the rewritten target, or None if the link is external or
        invalid (error recorded)."""
        if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("#"):
            return None
        anchor = ""
        path = target
        if "#" in target:
            path, anchor = target.split("#", 1)
            anchor = "#" + anchor
        if path == "":
            return None
        page_dir = os.path.dirname(self.page.rel_md)
        resolved = os.path.normpath(os.path.join(page_dir, path)).replace("\\", "/")
        self.page.links.append((self.page.rel_md, resolved, anchor.lstrip("#")))
        if resolved.endswith(".md"):
            return "@@PAGE@@:" + resolved + anchor
        return None

    def render_inline(self, text):
        codes = []

        def stash_code(match):
            codes.append(html.escape(match.group(1)))
            return "\x00%d\x00" % (len(codes) - 1)

        text = re.sub(r"`([^`]+)`", stash_code, text)
        text = html.escape(text, quote=False)

        def image(match):
            alt, target = match.group(1), match.group(2)
            rewritten = self.resolve_link(target)
            if rewritten and rewritten.startswith("@@PAGE@@:"):
                self.add_error("image target is a markdown page: %s" % target)
                return match.group(0)
            page_dir = os.path.dirname(self.page.rel_md)
            resolved = os.path.normpath(os.path.join(page_dir, target)).replace("\\", "/")
            self.page.links.append((self.page.rel_md, resolved, ""))
            return '<img src="%s" alt="%s" loading="lazy">' % (html.escape(rewritten or target, quote=True), alt)

        def link(match):
            label, target = match.group(1), match.group(2)
            if target.startswith("#"):
                return '<a href="%s">%s</a>' % (html.escape(target, quote=True), label)
            if re.match(r"^[a-z][a-z0-9+.-]*:", target):
                return '<a href="%s">%s</a>' % (html.escape(target, quote=True), label)
            rewritten = self.resolve_link(target)
            if rewritten is None:
                self.add_error("unresolved link target: %s" % target)
                return label
            return '<a href="%s">%s</a>' % (html.escape(rewritten, quote=True), label)

        text = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)", image, text)
        text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)", link, text)
        text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
        text = re.sub(r"(?<![*\w])\*([^*\n]+)\*(?![*\w])", r"<em>\1</em>", text)

        def restore(match):
            return "<code>%s</code>" % codes[int(match.group(1))]

        text = re.sub("\x00(\\d+)\x00", restore, text)
        return text

    def plain_text(self, text):
        text = re.sub(r"`([^`]+)`", r"\1", text)
        text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
        text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
        text = re.sub(r"\*\*([^*]+)\*\*", r"\1", text)
        text = re.sub(r"\*([^*]+)\*", r"\1", text)
        return text.strip()

    def heading(self, level, text):
        plain = self.plain_text(text)
        if level == 1:
            self.page.title = plain
        anchor = slugify(plain, self.seen_ids)
        if level in (2, 3):
            self.page.toc.append((level, anchor, plain))
        return '<h%d id="%s">%s</h%d>' % (level, anchor, self.render_inline(text), level)

    def table(self, lines):
        def split_row(line):
            line = line.strip()
            if line.startswith("|"):
                line = line[1:]
            if line.endswith("|"):
                line = line[:-1]
            return [cell.strip() for cell in line.split("|")]

        header = split_row(lines[0])
        aligns = []
        for cell in split_row(lines[1]):
            cell = cell.strip()
            left = cell.startswith(":")
            right = cell.endswith(":")
            if left and right:
                aligns.append(' align="center"')
            elif right:
                aligns.append(' align="right"')
            else:
                aligns.append("")
        out = ['<div class="table-wrap">', "<table>", "<thead>", "<tr>"]
        for i, cell in enumerate(header):
            align = aligns[i] if i < len(aligns) else ""
            out.append("<th%s>%s</th>" % (align, self.render_inline(cell)))
        out += ["</tr>", "</thead>", "<tbody>"]
        for line in lines[2:]:
            cells = split_row(line)
            out.append("<tr>")
            for i, cell in enumerate(cells):
                align = aligns[i] if i < len(aligns) else ""
                out.append("<td%s>%s</td>" % (align, self.render_inline(cell)))
            out.append("</tr>")
        out += ["</tbody>", "</table>", "</div>"]
        return "\n".join(out)

    def parse_list(self, lines, start):
        """Parse a list starting at lines[start]. Returns (html, next_index)."""
        base_indent = len(lines[start]) - len(lines[start].lstrip())
        items = []
        i = start
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                break
            indent = len(line) - len(line.lstrip())
            if indent < base_indent:
                break
            m = re.match(r"\s*([-*]|\d+\.)\s+(.*)$", line)
            if not m:
                break
            if indent == base_indent:
                kind = "ol" if m.group(1)[0].isdigit() else "ul"
                items.append([m.group(2), [], kind])
                i += 1
            else:
                if not items:
                    break
                child_items, i = self.parse_list_items(lines, i, indent)
                items[-1][1].extend(child_items)
        return self.render_nested(items), i

    def parse_list_items(self, lines, start, base_indent):
        items = []
        i = start
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                break
            indent = len(line) - len(line.lstrip())
            if indent < base_indent:
                break
            m = re.match(r"\s*([-*]|\d+\.)\s+(.*)$", line)
            if not m:
                break
            if indent == base_indent:
                kind = "ol" if m.group(1)[0].isdigit() else "ul"
                items.append([m.group(2), [], kind])
                i += 1
            else:
                child_items, i = self.parse_list_items(lines, i, indent)
                items[-1][1].extend(child_items)
        return items, i

    def render_nested(self, items):
        if not items:
            return ""
        ordered = items[0][2] == "ol"
        tag = "ol" if ordered else "ul"
        out = ["<%s>" % tag]
        for text, children, _ in items:
            out.append("<li>%s" % self.render_inline(text))
            if children:
                out.append(self.render_nested(children))
            out.append("</li>")
        out.append("</%s>" % tag)
        return "\n".join(out)

    def render(self, text):
        lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        out = []
        paragraph = []
        i = 0

        def flush_paragraph():
            if paragraph:
                joined = " ".join(paragraph).strip()
                if joined:
                    out.append("<p>%s</p>" % self.render_inline(joined))
                paragraph.clear()

        while i < len(lines):
            line = lines[i]
            stripped = line.strip()

            if stripped.startswith("```"):
                flush_paragraph()
                i += 1
                code = []
                while i < len(lines) and not lines[i].strip().startswith("```"):
                    code.append(lines[i])
                    i += 1
                i += 1
                out.append("<pre><code>%s</code></pre>" % html.escape("\n".join(code)))
                continue

            m = re.match(r"^(#{1,6})\s+(.*)$", stripped)
            if m:
                flush_paragraph()
                out.append(self.heading(len(m.group(1)), m.group(2).strip()))
                i += 1
                continue

            if "|" in line and i + 1 < len(lines) and re.match(r"^\s*\|?[\s:|-]+\|[\s:|-]*$", lines[i + 1]):
                flush_paragraph()
                table_lines = [line, lines[i + 1]]
                i += 2
                while i < len(lines) and "|" in lines[i] and lines[i].strip():
                    table_lines.append(lines[i])
                    i += 1
                out.append(self.table(table_lines))
                continue

            if re.match(r"^([-*_])\1{2,}\s*$", stripped) and not paragraph:
                out.append("<hr>")
                i += 1
                continue

            if stripped.startswith(">"):
                flush_paragraph()
                quote = []
                while i < len(lines) and lines[i].strip().startswith(">"):
                    quote.append(re.sub(r"^>\s?", "", lines[i].strip()))
                    i += 1
                inner = MarkdownRenderer(self.page, self.source_root, self.errors)
                out.append("<blockquote>%s</blockquote>" % inner.render(" ".join(quote)))
                continue

            if re.match(r"^\s*([-*]|\d+\.)\s+", line):
                flush_paragraph()
                block, i = self.parse_list(lines, i)
                out.append(block)
                continue

            if not stripped:
                flush_paragraph()
                i += 1
                continue

            paragraph.append(stripped)
            i += 1

        flush_paragraph()
        return "\n".join(out)


def collect_markdown(source_root):
    found = set()
    for dirpath, dirnames, filenames in os.walk(source_root):
        for name in filenames:
            if name.endswith(".md"):
                rel = os.path.relpath(os.path.join(dirpath, name), source_root)
                found.add(rel.replace("\\", "/"))
    return found


def build_sidebar(pages_by_md, active_md, base):
    parts = []
    for section, entries in NAV:
        parts.append('<div class="nav-section">%s</div>' % html.escape(section))
        for title, rel_md, sub in entries:
            page = pages_by_md[rel_md]
            cls = "nav-item sub" if sub else "nav-item"
            if rel_md == active_md:
                cls += " active"
            parts.append('<a class="%s" href="%s">%s</a>' % (cls, base + page.rel_html, html.escape(title)))
    return "\n            ".join(parts)


def build_pager(order, index, base):
    prev_link = order[index - 1] if index > 0 else None
    next_link = order[index + 1] if index + 1 < len(order) else None
    if not prev_link and not next_link:
        return ""
    parts = ['<div class="pager">']
    if prev_link:
        parts.append('<a class="prev" href="%s"><span class="label">'
                     '<span class="chevron">&lsaquo;</span>Previous</span>'
                     '<span class="title">%s</span></a>'
                     % (base + prev_link.rel_html, html.escape(prev_link.nav_title)))
    else:
        parts.append("<span></span>")
    if next_link:
        parts.append('<a class="next" href="%s"><span class="label">'
                     'Next<span class="chevron">&rsaquo;</span></span>'
                     '<span class="title">%s</span></a>'
                     % (base + next_link.rel_html, html.escape(next_link.nav_title)))
    else:
        parts.append("<span></span>")
    parts.append("</div>")
    return "\n            ".join(parts)


def build_toc(page):
    if len(page.toc) < 2:
        return ""
    parts = ['<nav class="toc">', '<div class="toc-title">On this page</div>']
    for level, anchor, text in page.toc:
        cls = "l3" if level == 3 else "l2"
        parts.append('<a class="%s" href="#%s">%s</a>' % (cls, anchor, html.escape(text)))
    parts.append("</nav>")
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description="Generate the uniOS wiki site.")
    parser.add_argument("--source", required=True, help="Markdown source directory (docs/reference)")
    parser.add_argument("--dest", required=True, help="Output directory")
    args = parser.parse_args()

    source_root = os.path.abspath(args.source)
    dest = os.path.abspath(args.dest)
    if not os.path.isdir(source_root):
        print("docs_site: source directory not found: %s" % source_root, file=sys.stderr)
        return 1

    errors = []
    nav_md = []
    pages_by_md = {}
    order = []
    for section, entries in NAV:
        for title, rel_md, _sub in entries:
            if rel_md in pages_by_md:
                errors.append("nav: duplicate entry %s" % rel_md)
                continue
            page = Page(rel_md, title, section)
            pages_by_md[rel_md] = page
            nav_md.append(rel_md)
            order.append(page)

    found = collect_markdown(source_root)
    for rel in sorted(found):
        # The top-level README.md is the GitHub-facing pointer to the wiki, not a page.
        if rel == "README.md":
            continue
        if rel not in pages_by_md:
            errors.append("nav: markdown file not listed in NAV: %s" % rel)
    for rel in nav_md:
        if rel not in found:
            errors.append("nav: listed page does not exist: %s" % rel)

    if errors:
        for err in errors:
            print("docs_site: %s" % err, file=sys.stderr)
        return 1

    for page in order:
        path = os.path.join(source_root, *page.rel_md.split("/"))
        with open(path, "r", encoding="utf-8") as handle:
            raw = handle.read()
        renderer = MarkdownRenderer(page, source_root, errors)
        page.html = renderer.render(raw)
        page.text = " ".join(
            re.sub(r"<[^>]+>", " ", re.sub(r"<pre><code>.*?</code></pre>", " ", page.html, flags=re.S)).split()
        )
        first_para = re.search(r"<p>(.*?)</p>", page.html, re.S)
        page.description = html.escape(
            re.sub(r"<[^>]+>", "", first_para.group(1))[:160] if first_para else page.title, quote=True
        )

    for page in order:
        for src, target, anchor in page.links:
            target_norm = target.replace("\\", "/")
            if target_norm.endswith(".md"):
                target_norm = re.sub(r"README\.md$", "index.html", target_norm)
                target_norm = re.sub(r"\.md$", ".html", target_norm)
                continue
            candidate = os.path.join(source_root, *target_norm.split("/"))
            if not os.path.exists(candidate) and not target_norm.startswith("http"):
                errors.append("%s: missing asset: %s" % (src, target_norm))

    html_by_md = {}
    for page in order:
        html_by_md[page.rel_md] = page

    for page in order:
        for src, target, anchor in page.links:
            if not target.endswith(".md"):
                continue
            target_page = pages_by_md.get(target)
            if target_page is None:
                errors.append("%s: broken link to %s" % (src, target))
                continue
            if anchor:
                ids = {item[1] for item in target_page.toc}
                ids.update(re.findall(r'id="([^"]+)"', target_page.html))
                if anchor not in ids:
                    errors.append("%s: unknown anchor #%s in %s" % (src, anchor, target))

    if errors:
        for err in errors:
            print("docs_site: %s" % err, file=sys.stderr)
        return 1

    for page in order:
        def fix_page_links(match):
            marker = match.group(1)
            target_md = marker.split("#")[0]
            target_page = pages_by_md[target_md]
            wbase = "../" * page.rel_html.count("/")
            href = wbase + target_page.rel_html
            if "#" in marker:
                href += "#" + marker.split("#", 1)[1]
            return 'href="%s"' % href

        page.html = re.sub(r'href="@@PAGE@@:([^"]+)"', fix_page_links, page.html)

    os.makedirs(dest, exist_ok=True)
    for page in order:
        # base reaches the site root (landing page); wbase reaches the wiki root.
        base = "../" * (page.rel_html.count("/") + 1)
        wbase = "../" * page.rel_html.count("/")
        index = order.index(page)
        sidebar = build_sidebar(pages_by_md, page.rel_md, wbase)
        toc = build_toc(page)
        pager = build_pager(order, index, wbase)
        document = PAGE_TEMPLATE.format(
            title=html.escape(page.nav_title if page.rel_md != "index.md" else "uniOS Wiki", quote=True),
            description=page.description,
            base=base,
            wbase=wbase,
            github_docs=GITHUB_TREE_DOCS,
            github_repo=GITHUB_REPO,
            sidebar=sidebar,
            toc_class=" has-toc" if toc else "",
            article=page.html,
            toc=toc,
            pager=pager,
        )
        out_path = os.path.join(dest, *page.rel_html.split("/"))
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(document)

    with open(os.path.join(dest, "wiki.css"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(CSS)
    with open(os.path.join(dest, "wiki.js"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(JS)

    search_entries = []
    for page in order:
        headings = " . ".join(text for _level, _anchor, text in page.toc)
        body = (page.title + " . " + headings + " . " + page.text)[:6000]
        search_entries.append({
            "u": page.rel_html,
            "t": page.title,
            "s": page.section,
            "b": body,
        })
    with open(os.path.join(dest, "search_index.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(search_entries, handle, separators=(",", ":"))

    print("docs_site: rendered %d pages -> %s" % (len(order), dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
