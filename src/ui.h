/*
 * ui.h - the embedded web UI page
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_UI_H
#define FORGECTRL_UI_H

/* The complete single-page UI served at "/" (self-contained HTML/CSS/JS,
 * no external assets), NUL-terminated. Generated at build time from
 * src/ui/ by src/ui/embed.cmake; the token placeholder in it is what
 * main.c substitutes when serving. */
extern const char index_html[];

#endif
