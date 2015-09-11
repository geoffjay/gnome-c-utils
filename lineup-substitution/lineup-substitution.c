/*
 * This file is part of gnome-c-utils.
 *
 * Copyright © 2015 Sébastien Wilmet <swilmet@gnome.org>
 *
 * gnome-c-utils is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gnome-c-utils is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gnome-c-utils.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Do a substitution and at the same time keep a good alignment of parameters on
 * the parenthesis.
 *
 * Usage: lineup-substitution <search-text> <replacement> <file>
 * WARNING: the script modifies <file>!
 *
 * Example:
 *
 * function_call (param1,
 *                param2,
 *                param3);
 *
 * $ lineup-substitution function_call another_beautiful_name file.c
 *
 * another_beautiful_name (param1,
 *                         param2,
 *                         param3);
 *
 * The file is opened, all occurrences of <search-text> are replaced by
 * <replacement> while still keeping a good alignment on the following lines.
 * Then the file is saved. Make sure you have a copy of <file> before running
 * the script. The best is to have it in a version control system like Git to
 * see the diff afterwards.
 *
 * The search is case sensitive, regular expressions are *not* supported, and it
 * does *not* try to match only at word boundaries (although it would be easy to
 * add such an option).
 *
 * Before replacing an occurrence, the script searches if (1) an opening
 * parenthesis is present further on the same line and (2) the following lines
 * are aligned on the parenthesis. If it is the case, then the alignment is
 * adjusted.
 *
 * The script works with both tabs and spaces. It just sees what is used before
 * adjusting the alignment, and inserts tabs and/or spaces accordingly. Run the
 * script on sample.c to see the result.
 *
 * The script doesn't fix broken alignment. It assumes that the code is
 * initially well indented. Fixing broken alignment is a harder problem to
 * solve.
 *
 * Further background on why this script has been written:
 * https://mail.gnome.org/archives/desktop-devel-list/2015-September/msg00020.html
 */

/* Note: yes, this script uses GTK+ and GtkSourceView, because
 * GtkSourceBuffer/GtkTextBuffer are good at navigating through text and editing
 * it. Another reason is because I'm familiar with those APIs, so it was easier
 * for me to write the code. And in the future, this code might be useful for a
 * graphical text editor based on GtkSourceView.
 */
#include <gtksourceview/gtksource.h>
#include <stdlib.h>
#include <locale.h>

typedef struct _Sub Sub;
struct _Sub
{
  gchar *search_text;
  gchar *replacement;
  GtkSourceFile *file;
  GtkSourceBuffer *buffer;

  /* Used to call gtk_source_view_get_visual_column(), so tabs are supported for
   * free.
   */
  GtkSourceView *view;
};

static Sub *
sub_new (const gchar *search_text,
         const gchar *replacement,
         const gchar *filename)
{
  Sub *sub = g_new0 (Sub, 1);
  GFile *location;

  g_assert (search_text != NULL);
  g_assert (search_text[0] != '\0');
  g_assert (replacement != NULL);
  g_assert (filename != NULL);
  g_assert (filename[0] != '\0');

  sub->search_text = g_strdup (search_text);
  sub->replacement = g_strdup (replacement);

  sub->file = gtk_source_file_new ();

  location = g_file_new_for_commandline_arg (filename);
  gtk_source_file_set_location (sub->file, location);
  g_object_unref (location);

  sub->buffer = gtk_source_buffer_new (NULL);
  sub->view = GTK_SOURCE_VIEW (gtk_source_view_new_with_buffer (sub->buffer));
  g_object_ref_sink (sub->view);

  return sub;
}

static void
sub_free (Sub *sub)
{
  if (sub != NULL)
    {
      g_free (sub->search_text);
      g_free (sub->replacement);
      g_clear_object (&sub->file);
      g_clear_object (&sub->buffer);
      g_clear_object (&sub->view);

      g_free (sub);
    }
}

static void
save_cb (GtkSourceFileSaver *saver,
         GAsyncResult       *result,
         Sub                *sub)
{
  GError *error = NULL;

  gtk_source_file_saver_save_finish (saver, result, &error);
  g_object_unref (saver);

  if (error != NULL)
    g_error ("Error when saving file: %s", error->message);

  gtk_main_quit ();
}

static void
save_file (Sub *sub)
{
  GtkSourceFileSaver *saver;

  saver = gtk_source_file_saver_new (sub->buffer, sub->file);

  gtk_source_file_saver_save_async (saver,
                                    G_PRIORITY_HIGH,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    (GAsyncReadyCallback) save_cb,
                                    sub);
}

static gint
get_parenthesis_column (Sub               *sub,
                        const GtkTextIter *iter)
{
  GtkTextIter match_end;
  GtkTextIter limit;

  limit = *iter;
  if (!gtk_text_iter_ends_line (&limit))
    gtk_text_iter_forward_to_line_end (&limit);

  if (gtk_text_iter_forward_search (iter,
                                    "(",
                                    GTK_TEXT_SEARCH_TEXT_ONLY,
                                    NULL,
                                    &match_end,
                                    &limit))
    return gtk_source_view_get_visual_column (sub->view, &match_end);

  return -1;
}

static void
forward_iter_to_text_start (GtkTextIter *iter)
{
  g_assert (gtk_text_iter_starts_line (iter));

  while (!gtk_text_iter_is_end (iter) &&
         !gtk_text_iter_ends_line (iter))
    {
      gunichar cur_char;

      cur_char = gtk_text_iter_get_char (iter);
      if (!g_unichar_isspace (cur_char))
        break;

      gtk_text_iter_forward_char (iter);
    }
}

static gint
get_text_start_column (Sub               *sub,
                       const GtkTextIter *start_iter)
{
  GtkTextIter iter;

  g_assert (gtk_text_iter_starts_line (start_iter));

  iter = *start_iter;
  forward_iter_to_text_start (&iter);

  if (gtk_text_iter_is_end (&iter) ||
      gtk_text_iter_ends_line (&iter))
    return -1;

  return gtk_source_view_get_visual_column (sub->view, &iter);
}

static gboolean
indentation_contains_tab (const GtkTextIter *line_iter)
{
  GtkTextIter iter;

  iter = *line_iter;
  gtk_text_iter_set_line_offset (&iter, 0);

  while (!gtk_text_iter_is_end (&iter) &&
         !gtk_text_iter_ends_line (&iter))
    {
      gunichar cur_char;

      cur_char = gtk_text_iter_get_char (&iter);
      if (cur_char == '\t')
        return TRUE;

      if (!g_unichar_isspace (cur_char))
        break;

      gtk_text_iter_forward_char (&iter);
    }

  return FALSE;
}

static void
adjust_alignment (Sub         *sub,
                  GtkTextIter *iter)
{
  GtkTextIter start_text_iter;
  gint new_length;
  gboolean tabs;

  g_assert (gtk_text_iter_starts_line (iter));

  start_text_iter = *iter;
  forward_iter_to_text_start (&start_text_iter);
  g_assert (!gtk_text_iter_is_end (&start_text_iter));
  g_assert (!gtk_text_iter_ends_line (&start_text_iter));

  new_length = (get_text_start_column (sub, iter) -
                g_utf8_strlen (sub->search_text, -1) +
                g_utf8_strlen (sub->replacement, -1));
  g_assert_cmpint (new_length, >=, 0);

  tabs = indentation_contains_tab (iter);

  gtk_text_buffer_delete (GTK_TEXT_BUFFER (sub->buffer),
                          iter,
                          &start_text_iter);

  if (tabs)
    {
      gchar *tabs;
      gchar *spaces;
      gchar *indentation;

      tabs = g_strnfill (new_length / 8, '\t');
      spaces = g_strnfill (new_length % 8, ' ');
      indentation = g_strdup_printf ("%s%s", tabs, spaces);

      gtk_text_buffer_insert (GTK_TEXT_BUFFER (sub->buffer),
                              iter,
                              indentation,
                              -1);

      g_free (tabs);
      g_free (spaces);
      g_free (indentation);
    }
  else
    {
      gchar *spaces;

      spaces = g_strnfill (new_length, ' ');

      gtk_text_buffer_insert (GTK_TEXT_BUFFER (sub->buffer),
                              iter,
                              spaces,
                              new_length);

      g_free (spaces);
    }

  gtk_text_iter_set_line_offset (iter, 0);
}

static void
replace (Sub                    *sub,
         GtkSourceSearchContext *search_context,
         const GtkTextIter      *match_start,
         GtkTextIter            *match_end)
{
  GError *error = NULL;
  GtkTextMark *mark;
  gint parenthesis_column;

  mark = gtk_text_buffer_create_mark (GTK_TEXT_BUFFER (sub->buffer),
                                      NULL,
                                      match_end,
                                      FALSE);

  parenthesis_column = get_parenthesis_column (sub, match_end);

  gtk_source_search_context_replace (search_context,
                                     match_start,
                                     match_end,
                                     sub->replacement, -1,
                                     &error);

  if (error != NULL)
    g_error ("Error when doing the substitution: %s", error->message);

  gtk_text_buffer_get_iter_at_mark (GTK_TEXT_BUFFER (sub->buffer),
                                    match_end,
                                    mark);

  if (parenthesis_column != -1)
    {
      GtkTextIter next_line;

      next_line = *match_end;
      while (gtk_text_iter_forward_line (&next_line))
        {
          gint text_start_column;

          text_start_column = get_text_start_column (sub, &next_line);

          if (text_start_column == parenthesis_column)
            adjust_alignment (sub, &next_line);
          else
            break;
        }
    }

  gtk_text_buffer_get_iter_at_mark (GTK_TEXT_BUFFER (sub->buffer),
                                    match_end,
                                    mark);

  gtk_text_buffer_delete_mark (GTK_TEXT_BUFFER (sub->buffer), mark);
}

static void
do_substitution (Sub *sub)
{
  GtkSourceSearchSettings *search_settings;
  GtkSourceSearchContext *search_context;
  GtkTextIter iter;
  GtkTextIter match_start;
  GtkTextIter match_end;

  search_settings = gtk_source_search_settings_new ();
  gtk_source_search_settings_set_search_text (search_settings, sub->search_text);
  gtk_source_search_settings_set_case_sensitive (search_settings, TRUE);

  search_context = gtk_source_search_context_new (sub->buffer, search_settings);

  gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (sub->buffer), &iter);

  while (gtk_source_search_context_forward (search_context,
                                            &iter,
                                            &match_start,
                                            &match_end))
    {
      replace (sub, search_context, &match_start, &match_end);
      iter = match_end;
    }

  g_object_unref (search_settings);
  g_object_unref (search_context);
}

static void
load_cb (GtkSourceFileLoader *loader,
         GAsyncResult        *result,
         Sub                 *sub)
{
  GError *error = NULL;

  gtk_source_file_loader_load_finish (loader, result, &error);
  g_object_unref (loader);

  if (error != NULL)
    g_error ("Error when loading file: %s", error->message);

  do_substitution (sub);
  save_file (sub);
}

static void
sub_launch (Sub *sub)
{
  GtkSourceFileLoader *loader;

  loader = gtk_source_file_loader_new (sub->buffer, sub->file);

  gtk_source_file_loader_load_async (loader,
                                     G_PRIORITY_HIGH,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     (GAsyncReadyCallback) load_cb,
                                     sub);
}

gint
main (gint   argc,
      gchar *argv[])
{
  const gchar *search_text;
  const gchar *replacement;
  const gchar *filename;
  Sub *sub;

  setlocale (LC_ALL, "");

  gtk_init (NULL, NULL);

  if (argc != 4)
    {
      g_printerr ("Usage: %s <search-text> <replacement> <file>\n", argv[0]);
      g_printerr ("WARNING: the script modifies <file>!\n");
      return EXIT_FAILURE;
    }

  search_text = argv[1];
  replacement = argv[2];
  filename = argv[3];

  sub = sub_new (search_text, replacement, filename);

  sub_launch (sub);

  gtk_main ();

  sub_free (sub);
  return EXIT_SUCCESS;
}