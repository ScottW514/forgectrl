/*
 * ui.h - the embedded web UI page
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_UI_H
#define FORGECTRL_UI_H

/* The complete single-page UI served at "/" (self-contained HTML/CSS/JS,
 * no external assets), gzip-compressed at build time from src/ui/ by
 * src/ui/embed.cmake: the compressed bytes, how many there are, and the
 * size of the page they inflate to. main.c inflates it once and
 * substitutes the token placeholder in the result before serving. */
extern const unsigned char index_html_gz[];
extern const unsigned int index_html_gz_len;
extern const unsigned int index_html_len;

#endif
