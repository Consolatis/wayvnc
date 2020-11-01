/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * Acknowledgements: Reading Josef Gajdusek's wvnc code helped me understand
 * how to use the xkbcommon API to interface with the wayland virtual keyboard
 * interface.
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>

#include "virtual-keyboard-unstable-v1.h"
#include "keyboard.h"
#include "shm.h"
#include "logging.h"
#include "intset.h"

#define MAYBE_UNUSED __attribute__((unused))

struct table_entry {
	xkb_keysym_t symbol;
	xkb_keycode_t code;
	int level;
};

static void append_entry(struct keyboard* self, xkb_keysym_t symbol,
                         xkb_keycode_t code, int level)
{
	if (self->lookup_table_size <= self->lookup_table_length) {
		size_t new_size = self->lookup_table_size * 2;
		struct table_entry* table =
			realloc(self->lookup_table, new_size * sizeof(*table));
		if (!table)
			return; // TODO: Report this

		self->lookup_table_size = new_size;
		self->lookup_table = table;
	}

	struct table_entry* entry =
		&self->lookup_table[self->lookup_table_length++];

	entry->symbol = symbol;
	entry->code = code;
	entry->level = level;
}

static void key_iter(struct xkb_keymap* map, xkb_keycode_t code, void* userdata)
{
	struct keyboard* self = userdata;

	size_t n_levels = xkb_keymap_num_levels_for_key(map, code, 0);

	for (size_t level = 0; level < n_levels; level++) {
		const xkb_keysym_t* symbols;
		size_t n_syms = xkb_keymap_key_get_syms_by_level(map, code, 0,
				                                 level,
				                                 &symbols);

		for (size_t sym_idx = 0; sym_idx < n_syms; sym_idx++)
			append_entry(self, symbols[sym_idx], code, level);
	}
}

static int compare_symbols(const void* a, const void* b)
{
	const struct table_entry* x = a;
	const struct table_entry* y = b;

	if (x->symbol == y->symbol)
		return x->level < y->level ? -1 : x->level > y->level;

	return x->symbol < y->symbol ? -1 : x->symbol > y->symbol;
}

static int compare_symbols2(const void* a, const void* b)
{
	const struct table_entry* x = a;
	const struct table_entry* y = b;

	return x->symbol < y->symbol ? -1 : x->symbol > y->symbol;
}

static int create_lookup_table(struct keyboard* self)
{
	self->lookup_table_length = 0;
	self->lookup_table_size = 128;

	self->lookup_table =
		malloc(self->lookup_table_size * sizeof(*self->lookup_table));
	if (!self->lookup_table)
		return -1;

	xkb_keymap_key_for_each(self->keymap, key_iter, self);

	qsort(self->lookup_table, self->lookup_table_length,
	      sizeof(*self->lookup_table), compare_symbols);

	return 0;
}

static char* get_symbol_name(xkb_keysym_t sym, char* dst, size_t size)
{
	if (xkb_keysym_get_name(sym, dst, size) >= 0)
		return dst;

	snprintf(dst, size, "UNKNOWN (%x)", sym);
	return dst;
}

static void keyboard__dump_entry(const struct keyboard* self,
                                 const struct table_entry* entry)
{
	char sym_name[256];
	get_symbol_name(entry->symbol, sym_name, sizeof(sym_name));

	const char* code_name MAYBE_UNUSED =
		xkb_keymap_key_get_name(self->keymap, entry->code);

	bool is_pressed MAYBE_UNUSED =
		intset_is_set(&self->key_state, entry->code);

	log_debug("symbol=%s level=%d code=%s %s\n", sym_name, entry->level,
	          code_name, is_pressed ? "pressed" : "released");
}

void keyboard_dump_lookup_table(const struct keyboard* self)
{
	for (size_t i = 0; i < self->lookup_table_length; i++)
		keyboard__dump_entry(self, &self->lookup_table[i]);
}

int keyboard_init(struct keyboard* self, const char* layout, const char* variant)
{
	self->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!self->context)
		return -1;

	if (intset_init(&self->key_state, 0) < 0)
		goto key_state_failure;

	struct xkb_rule_names rule_names = {
		.layout = layout,
		.model = "pc105",
		.variant = variant,
	};

	self->keymap = xkb_keymap_new_from_names(self->context, &rule_names, 0);
	if (!self->keymap)
		goto keymap_failure;

	if (create_lookup_table(self) < 0)
		goto table_failure;

//	keyboard_dump_lookup_table(self);

	char* keymap_string =
		xkb_keymap_get_as_string(self->keymap,
		                         XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!keymap_string)
		goto keymap_string_failure;

	size_t keymap_size = strlen(keymap_string) + 1;

	int keymap_fd = shm_alloc_fd(keymap_size);
	if (keymap_fd < 0)
		goto fd_failure;

	size_t written = 0;
	while (written < keymap_size) {
		ssize_t ret = write(keymap_fd, keymap_string + written, keymap_size - written);
		if (ret == -1 && errno == EINTR)
			continue;
		if (ret == -1)
			goto write_failure;
		written += ret;
	}

	free(keymap_string);

	zwp_virtual_keyboard_v1_keymap(self->virtual_keyboard,
	                               WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
	                               keymap_fd, keymap_size);

	close(keymap_fd);

	return 0;

write_failure:
	close(keymap_fd);
fd_failure:
	free(keymap_string);
keymap_string_failure:
	free(self->lookup_table);
table_failure:
	xkb_keymap_unref(self->keymap);
keymap_failure:
	intset_destroy(&self->key_state);
key_state_failure:
	xkb_context_unref(self->context);
	return -1;
}

void keyboard_destroy(struct keyboard* self)
{
	free(self->lookup_table);
	xkb_keymap_unref(self->keymap);
	intset_destroy(&self->key_state);
	xkb_context_unref(self->context);
}

struct table_entry* keyboard_find_symbol(const struct keyboard* self,
                                         xkb_keysym_t symbol)
{
	struct table_entry cmp = { .symbol = symbol };

	struct table_entry* entry =
		bsearch(&cmp, self->lookup_table, self->lookup_table_length,
		        sizeof(*self->lookup_table), compare_symbols2);

	if (!entry)
		return NULL;

	while (entry != self->lookup_table && (entry - 1)->symbol == symbol)
		--entry;

	return entry;
}

void keyboard_feed(struct keyboard* self, xkb_keysym_t symbol, bool is_pressed)
{
	uint32_t mods = 0;

	struct table_entry* entry = keyboard_find_symbol(self, symbol);
	if (!entry) {
		char name[256];
		log_error("Failed to look up keyboard symbol: %s\n",
		          get_symbol_name(symbol, name, sizeof(name)));
		return;
	}

	xkb_keymap_key_get_mods_for_level(self->keymap, entry->code,
			/* layout index */ 0, entry->level, &mods, 1);

	bool was_pressed = intset_is_set(&self->key_state, entry->code);
	if (was_pressed == is_pressed)
		return;

	if (is_pressed)
		intset_set(&self->key_state, entry->code);
	else
		intset_clear(&self->key_state, entry->code);

#ifndef NDEBUG
	keyboard__dump_entry(self, entry);
#endif

	enum xkb_state_component depressed, latched, locked, group;
	depressed = 0;
	latched = mods;
	locked = 0;
	group = 0;

	// TODO: This could cause some synchronisation problems with other
	// keyboards in the seat.
	zwp_virtual_keyboard_v1_modifiers(self->virtual_keyboard, depressed,
	                                  latched, locked, group);

	// TODO: Handle errors
	zwp_virtual_keyboard_v1_key(self->virtual_keyboard, 0, entry->code - 8,
	                            is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                                       : WL_KEYBOARD_KEY_STATE_RELEASED);
}
