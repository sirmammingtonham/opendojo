-- OpenDojo Cloud — add the "Other" drill category.
--
-- A catch-all tag for drills that don't fit the existing taxonomy
-- (reaction / option select / fuzzy guard / punishment / throw break).
-- drill_categories is an additive lookup table, so a single insert is
-- enough; the Edge Function's ALLOWED_CATEGORIES set and the mod's
-- kCategories array are mirrored by hand (see their comments).
--
-- Idempotent so re-running migrations against a DB that already has the
-- row (e.g. a hand-patched staging instance) is a no-op.

insert into drill_categories (id, label) values
    ('other', 'Other')
on conflict (id) do nothing;
