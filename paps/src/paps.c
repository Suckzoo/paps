/* Pango
 * paps.c: A postscript printing program using pango.
 *
 * Copyright (C) 2002, 2005 Dov Grobgeld <dov.grobgeld@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */


#include <pango/pango.h>
#include "libpaps.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BUFSIZE 1024
#define HEADER_FONT_SCALE 12

typedef enum {
    PAPER_TYPE_A4 = 0,
    PAPER_TYPE_US_LETTER = 1,
    PAPER_TYPE_US_LEGAL = 2
} paper_type_t ;

typedef struct  {
    double width;
    double height;
} paper_size_t;

const paper_size_t paper_sizes[] = {
    { 595.28, 841.89}, /* A4 */
    { 612, 792},       /* US letter */
    { 612, 1008}      /* US legal */
};

typedef struct {
  double pt_to_pixel;
  double pixel_to_pt;
  int column_width;
  int column_height;
  int num_columns;
  int gutter_width;  /* These are all in postscript points=1/72 inch... */
  int top_margin;  
  int bottom_margin;
  int left_margin;
  int right_margin;
  int page_width;
  int page_height;
  int header_ypos;
  int header_sep;
  int header_height;
  int footer_height;
  gboolean do_draw_header;
  gboolean do_draw_footer;
  gboolean do_duplex;
  gboolean do_tumble;
  gboolean do_landscape;
  gboolean do_justify;
  gboolean do_separation_line;
  gboolean do_draw_contour;
  PangoDirection pango_dir;
  gchar *filename;
  gchar *header_font_desc;
} page_layout_t;

typedef struct {
  char *text;
  int length;
} para_t;

typedef struct {
  PangoLayoutLine *pango_line;
  PangoRectangle logical_rect;
  PangoRectangle ink_rect;
  int formfeed;
} LineLink;

typedef struct _Paragraph Paragraph;

/* Structure representing a paragraph
 */
struct _Paragraph {
  char *text;
  int length;
  int height;   /* Height, in pixels */
  int formfeed;
  PangoLayout *layout;
};

/* Information passed in user data when drawing outlines */
GList        *split_paragraphs_into_lines  (GList           *paragraphs);
static char  *read_file                    (FILE            *file,
                                            GIConv           handle);
static GList *split_text_into_paragraphs   (PangoContext    *pango_context,
                                            page_layout_t   *page_layout,
                                            int              paint_width,
                                            char            *text);
static int    output_pages                 (FILE            *OUT,
                                            GList           *pango_lines,
                                            page_layout_t   *page_layout,
                                            gboolean         need_header,
                                            PangoContext    *pango_context);
static void   print_postscript_header      (FILE            *OUT,
                                            const char      *title,
                                            page_layout_t   *page_layout);
static void   print_postscript_trailer     (FILE            *OUT,
                                            int              num_pages);
static void   eject_column                 (FILE            *OUT,
                                            page_layout_t   *page_layout,
                                            int              column_idx);
static void   eject_page                   (FILE            *OUT);
static void   start_page                   (FILE            *OUT,
                                            int              page_idx);
static void   draw_line_to_page            (FILE            *OUT,
                                            int              column_idx,
                                            int              column_pos,
                                            page_layout_t   *page_layout,
                                            PangoLayoutLine *line);
static int    draw_page_header_line_to_page(FILE            *OUT,
                                            gboolean         is_footer,
                                            page_layout_t   *page_layout,
                                            PangoContext    *ctx,
                                            int              page);

// Fonts are three character symbols in an alphabet composing of
// the following characters:
//
//   First character:  a-zA-Z@_.,!-~`'"
//   Rest of chars:    like first + 0-9
//
// Care is taken that no keywords are overwritten, e.g. def, end.
GString *ps_font_def_string = NULL;
GString *ps_pages_string = NULL;
int last_char_idx = 0;
double last_pos_y = -1;
double last_pos_x = -1;
paps_t *paps;
paper_type_t paper_type = PAPER_TYPE_A4;

#define CASE(s) if (strcmp(S_, s) == 0)

static gboolean
_paps_arg_paper_cb(const char *option_name,
                   const char *value,
                   gpointer    data)
{
  gboolean retval = TRUE;
  
  if (value && *value)
    {
      if (g_ascii_strcasecmp(value, "legal") == 0)
        paper_type = PAPER_TYPE_US_LEGAL;
      else if (g_ascii_strcasecmp(value, "letter") == 0)
        paper_type = PAPER_TYPE_US_LETTER;
      else if (g_ascii_strcasecmp(value, "a4") == 0)
        paper_type = PAPER_TYPE_A4;
      else {
        retval = FALSE;
        fprintf(stderr, "Unknown page size name: %s.\n", value);
      }
    }
  else
    {
      fprintf(stderr, "You must specify page size.\n");
      retval = FALSE;
    }
  
  return retval;
}

int main(int argc, char *argv[])
{
  gboolean do_landscape = FALSE, do_rtl = FALSE, do_justify = FALSE, do_draw_header = FALSE;
  int num_columns = 1, font_scale = 12;
  int top_margin = 36, bottom_margin = 36, right_margin = 36, left_margin = 36;
  char *font_family = "Monospace", *encoding = NULL;
  GOptionContext *ctxt = g_option_context_new("[text file]");
  GOptionEntry entries[] = {
    {"landscape", 0, 0, G_OPTION_ARG_NONE, &do_landscape, "Landscape output. (Default: portrait)", NULL},
    {"columns", 0, 0, G_OPTION_ARG_INT, &num_columns, "Number of columns output. (Default: 1)", "NUM"},
    {"font-scale", 0, 0, G_OPTION_ARG_INT, &font_scale, "Font scaling. (Default: 12)", "NUM"},
    {"family", 0, 0, G_OPTION_ARG_STRING, &font_family, "Pango FT2 font family. (Default: Monospace)", "FAMILY"},
    {"rtl", 0, 0, G_OPTION_ARG_NONE, &do_rtl, "Do rtl layout.", NULL},
    {"justify", 0, 0, G_OPTION_ARG_NONE, &do_justify, "Do justify the lines.", NULL},
    {"paper", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_paper_cb,
     "Choose paper size. Known paper sizes are legal,\n"
     "                          letter, a4. (Default: a4)", "PAPER"},
    {"bottom-margin", 0, 0, G_OPTION_ARG_INT, &bottom_margin, "Set bottom margin. (Default: 36)", "NUM"},
    {"top-margin", 0, 0, G_OPTION_ARG_INT, &top_margin, "Set top margin. (Default: 36)", "NUM"},
    {"right-margin", 0, 0, G_OPTION_ARG_INT, &right_margin, "Set right margin. (Default: 36)", "NUM"},
    {"left-margin", 0, 0, G_OPTION_ARG_INT, &left_margin, "Set left margin. (Default: 36)", "NUM"},
    {"header", 0, 0, G_OPTION_ARG_NONE, &do_draw_header, "Draw page header for each page.", NULL},
    {"encoding", 0, 0, G_OPTION_ARG_STRING, &encoding, "Assume the documentation encoding.", "ENCODING"},
    {NULL}
  };
  GError *error = NULL;
  char *filename_in;
  char *title;
  FILE *IN, *OUT = NULL;
  page_layout_t page_layout;
  char *text;
  GList *paragraphs;
  GList *pango_lines;
  PangoContext *pango_context;
  PangoFontDescription *font_description;
  PangoDirection pango_dir = PANGO_DIRECTION_LTR;
  int num_pages = 1;
  int gutter_width = 40;
  int total_gutter_width;
  int page_width = paper_sizes[0].width;
  int page_height = paper_sizes[0].height;
  int do_tumble = -1;   /* -1 means not initialized */
  int do_duplex = -1;
  gchar *paps_header = NULL;
  gchar *header_font_desc = "Monospace Bold 12";
  int header_sep = 20;
  GIConv cvh = NULL;

  /* Prerequisite when using glib. */
  g_type_init();
  
  g_option_context_add_main_entries(ctxt, entries, NULL);
  
  /* Parse command line */
  if (!g_option_context_parse(ctxt, &argc, &argv, &error))
    {
      fprintf(stderr, "Command line error: %s\n", error->message);
      exit(1);
    }
  
  if (do_rtl)
    pango_dir = PANGO_DIRECTION_RTL;
  
  if (argc > 1)
    {
      filename_in = argv[1];
      IN = fopen(filename_in, "rb");
      if (!IN)
        {
          fprintf(stderr, "Failed to open %s!\n", filename_in);
          exit(-1);
        }
    }
  else
    {
      filename_in = "stdin";
      IN = stdin;
    }
  title = filename_in;
  
  paps = paps_new();
  pango_context = paps_get_pango_context (paps);
  
  /* Setup pango */
  pango_context_set_language (pango_context, pango_language_from_string ("en_US"));
  pango_context_set_base_dir (pango_context, pango_dir);
  
  font_description = pango_font_description_new ();
  pango_font_description_set_family (font_description, g_strdup(font_family));
  pango_font_description_set_style (font_description, PANGO_STYLE_NORMAL);
  pango_font_description_set_variant (font_description, PANGO_VARIANT_NORMAL);
  pango_font_description_set_weight (font_description, PANGO_WEIGHT_NORMAL);
  pango_font_description_set_stretch (font_description, PANGO_STRETCH_NORMAL);
  pango_font_description_set_size (font_description, font_scale * PANGO_SCALE);

  pango_context_set_font_description (pango_context, font_description);

  /* Page layout */
  page_width = paper_sizes[(int)paper_type].width;
  page_height = paper_sizes[(int)paper_type].height;
  
  if (num_columns == 1)
    total_gutter_width = 0;
  else
    total_gutter_width = gutter_width * (num_columns - 1);
  if (do_landscape)
    {
      int tmp;
      tmp = page_width;
      page_width = page_height;
      page_height = tmp;
      if (do_tumble < 0)
        do_tumble = TRUE;
      if (do_duplex < 0)
        do_duplex = TRUE;
    }
  else
    {
      if (do_tumble < 0)
        do_tumble = TRUE;
      if (do_duplex < 0)
        do_duplex = TRUE;
    }
  
  page_layout.page_width = page_width;
  page_layout.page_height = page_height;
  page_layout.num_columns = num_columns;
  page_layout.left_margin = left_margin;
  page_layout.right_margin = right_margin;
  page_layout.gutter_width = gutter_width;
  page_layout.top_margin = top_margin;
  page_layout.bottom_margin = bottom_margin;
  page_layout.header_ypos = page_layout.top_margin;
  page_layout.header_height = 0;
  page_layout.footer_height = 0;
  if (do_draw_header)
    page_layout.header_sep =  header_sep;
  else
    page_layout.header_sep = 0;
    
  page_layout.column_height = page_height
                            - page_layout.top_margin
                            - page_layout.header_sep
                            - page_layout.bottom_margin;
  page_layout.column_width =  (page_layout.page_width 
                            - page_layout.left_margin - page_layout.right_margin
                            - total_gutter_width) / page_layout.num_columns;
  page_layout.pt_to_pixel = paps_postscript_points_to_pango(1)/PANGO_SCALE;
  page_layout.pixel_to_pt = 1.0/page_layout.pt_to_pixel;
  page_layout.do_separation_line = TRUE;
  page_layout.do_landscape = do_landscape;
  page_layout.do_justify = do_justify;
  page_layout.do_tumble = do_tumble;
  page_layout.do_duplex = do_duplex;
  page_layout.pango_dir = pango_dir;
  page_layout.filename = filename_in;
  page_layout.header_font_desc = header_font_desc;
  
  if (encoding != NULL)
    {
      cvh = g_iconv_open ("UTF-8", encoding);
      if (cvh == NULL)
        {
          fprintf(stderr, "%s: Invalid encoding: %s\n", g_get_prgname (), encoding);
          exit(-1);
        }
    }

  text = read_file(IN, cvh);

  if (encoding != NULL && cvh != NULL)
    g_iconv_close(cvh);

  paragraphs = split_text_into_paragraphs(pango_context,
                                          &page_layout,
                                          page_layout.column_width * page_layout.pt_to_pixel,
                                          text);
  pango_lines = split_paragraphs_into_lines(paragraphs);
  
  if (OUT == NULL)
    OUT = stdout;

  print_postscript_header(OUT, title, &page_layout);
  ps_pages_string = g_string_new("");
  
  num_pages = output_pages(OUT, pango_lines, &page_layout, do_draw_header, pango_context);

  paps_header = paps_get_postscript_header_strdup(paps);
  fprintf(OUT, "%s", paps_header);
  g_free(paps_header);

  fprintf(OUT, "%%%%EndPrologue\n");
  fprintf(OUT, "%s", ps_pages_string->str);
  print_postscript_trailer(OUT, num_pages);

  // Cleanup
  g_string_free(ps_pages_string, TRUE);
  g_option_context_free(ctxt);

  return 0;
}


/* Read an entire file into a string
 */
static char *
read_file (FILE   *file,
           GIConv  handle)
{
  GString *inbuf;
  char *text;
  char buffer[BUFSIZE];

  inbuf = g_string_new (NULL);
  while (1)
    {
      char *ib, *ob, obuffer[BUFSIZE * 6], *bp = fgets (buffer, BUFSIZE-1, file);
      gsize iblen, oblen;

      if (ferror (file))
        {
          fprintf(stderr, "%s: Error reading file.\n", g_get_prgname ());
          g_string_free (inbuf, TRUE);
          return NULL;
        }
      else if (bp == NULL)
        break;

      if (handle != NULL)
        {
          ib = bp;
          iblen = strlen (ib);
          ob = bp = obuffer;
          oblen = BUFSIZE * 6 - 1;
          if (g_iconv (handle, &ib, &iblen, &ob, &oblen) == -1)
            {
              fprintf (stderr, "%s: Error while converting strings.\n", g_get_prgname ());
              return NULL;
            }
          obuffer[BUFSIZE * 6 - 1 - oblen] = 0;
        }
      g_string_append (inbuf, bp);
    }

  fclose (file);

  /* Add a trailing new line if it is missing */
  if (inbuf->str[inbuf->len-1] != '\n')
    g_string_append(inbuf, "\n");

  text = inbuf->str;
  g_string_free (inbuf, FALSE);

  return text;
}

/* Take a UTF8 string and break it into paragraphs on \n characters
 */
static GList *
split_text_into_paragraphs (PangoContext *pango_context,
                            page_layout_t *page_layout,
                            int paint_width,  /* In pixels */
                            char *text)
{
  char *p = text;
  char *next;
  gunichar wc;
  GList *result = NULL;
  char *last_para = text;
  
  while (p != NULL && *p)
    {
      wc = g_utf8_get_char (p);
      next = g_utf8_next_char (p);
      if (wc == (gunichar)-1)
        {
          fprintf (stderr, "%s: Invalid character in input\n", g_get_prgname ());
          wc = 0;
        }
      if (!*p || !wc || wc == '\n' || wc == '\f')
        {
          Paragraph *para = g_new (Paragraph, 1);
          para->text = last_para;
          para->length = p - last_para;
          para->layout = pango_layout_new (pango_context);
          pango_layout_set_text (para->layout, para->text, para->length);
          pango_layout_set_justify (para->layout, page_layout->do_justify);
          pango_layout_set_alignment (para->layout,
                                      page_layout->pango_dir == PANGO_DIRECTION_LTR
                                      ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);
          pango_layout_set_width (para->layout, paint_width * PANGO_SCALE);
          para->height = 0;

          if (wc == '\f')
              para->formfeed = 1;
          else
              para->formfeed = 0;

          last_para = next;
            
          result = g_list_prepend (result, para);
        }
      if (!wc) /* incomplete character at end */
        break;
      p = next;
    }

  return g_list_reverse (result);
}

/* Split a list of paragraphs into a list of lines.
 */
GList *
split_paragraphs_into_lines(GList *paragraphs)
{
  GList *line_list = NULL;
  /* Read the file */

  /* Now split all the pagraphs into lines */
  GList *par_list;

  par_list = paragraphs;
  while(par_list)
    {
      int para_num_lines, i;
      LineLink *line_link;
      Paragraph *para = par_list->data;

      para_num_lines = pango_layout_get_line_count(para->layout);

      for (i=0; i<para_num_lines; i++)
        {
          PangoRectangle logical_rect, ink_rect;
          
          line_link = g_new(LineLink, 1);
          line_link->formfeed = 0;
          line_link->pango_line = pango_layout_get_line(para->layout, i);
          pango_layout_line_get_extents(line_link->pango_line,
                                        &ink_rect, &logical_rect);
          line_link->logical_rect = logical_rect;
          if (para->formfeed && i == (para_num_lines - 1))
              line_link->formfeed = 1;
          line_link->ink_rect = ink_rect;
          line_list = g_list_prepend(line_list, line_link);
        }

      par_list = par_list->next;
    }

  return g_list_reverse(line_list);
  
}

int
output_pages(FILE          *OUT,
             GList         *pango_lines,
             page_layout_t *page_layout,
             gboolean       need_header,
             PangoContext  *pango_context)
{
  int column_idx = 0;
  int column_y_pos = 0;
  int page_idx = 1;
  int pango_column_height = page_layout->column_height * page_layout->pt_to_pixel * PANGO_SCALE;
  LineLink *prev_line_link = NULL;

  start_page(OUT, page_idx);

  if (need_header)
    draw_page_header_line_to_page(OUT, FALSE, page_layout, pango_context, page_idx);

  while(pango_lines)
    {
      LineLink *line_link = pango_lines->data;
      PangoLayoutLine *line = line_link->pango_line;
      
      /* Check if we need to move to next column */
      if ((column_y_pos + line_link->logical_rect.height
           >= pango_column_height) ||
          (prev_line_link && prev_line_link->formfeed))
        {
          column_idx++;
          column_y_pos = 0;
          if (column_idx == page_layout->num_columns)
            {
              column_idx = 0;
              eject_page(OUT);
              page_idx++;
              start_page(OUT, page_idx);

              if (need_header)
                draw_page_header_line_to_page(OUT, FALSE, page_layout, pango_context, page_idx);
            }
          else
            {
              eject_column(OUT,
                           page_layout,
                           column_idx
                           );
            }
        }
      draw_line_to_page(OUT,
                        column_idx,
                        column_y_pos+line_link->logical_rect.height,
                        page_layout,
                        line);
      column_y_pos += line_link->logical_rect.height;
      
      pango_lines = pango_lines->next;
      prev_line_link = line_link;
    }
  eject_page(OUT);
  return page_idx;
}

void print_postscript_header(FILE *OUT,
                             const char *title,
                             page_layout_t *page_layout)
{
  const char *bool_name[2] = { "false", "true" };
  const char *orientation_names[2] = { "Portrait", "Landscape" };
  int bodytop = page_layout->header_ypos + page_layout->header_sep;
  int orientation = page_layout->page_width > page_layout->page_height;
  int bb_page_width = page_layout->page_width;
  int bb_page_height = page_layout->page_height;
  
  /* Keep bounding box non-rotated to make ggv happy */
  if (orientation)
    {
      int tmp = bb_page_width;
      bb_page_width = bb_page_height;
      bb_page_height = tmp;
    }
  
  fprintf(OUT,
          "%%!PS-Adobe-3.0\n"
          "%%%%Title: %s\n"
          "%%%%Creator: paps version 0.6.3 by Dov Grobgeld\n"
          "%%%%Pages: (atend)\n"
          "%%%%BoundingBox: 0 0 %d %d\n"
          "%%%%BeginProlog\n"
          "%%%%Orientation: %s\n"
          "/papsdict 1 dict def\n"
          "papsdict begin\n"
          "\n"
          "/inch {72 mul} bind def\n"
          "/mm {1 inch 25.4 div mul} bind def\n"
          "\n"
          "%% override setpagedevice if it is not defined\n"
          "/setpagedevice where {\n"
          "    pop %% get rid of its dictionary\n"
          "    /setpagesize { \n"
          "       3 dict begin\n"
          "         /pageheight exch def \n"
          "         /pagewidth exch def\n"
          "         /orientation 0 def\n"
          "         %% Exchange pagewidth and pageheight so that pagewidth is bigger\n"
          "         pagewidth pageheight gt {  \n"
          "             pagewidth\n"
          "             /pagewidth pageheight def\n"
          "             /pageheight exch def\n"
          "             /orientation 3 def\n"
          "         } if\n"
          "         2 dict\n"
          "         dup /PageSize [pagewidth pageheight] put\n"
          "         dup /Orientation orientation put\n"
          "         setpagedevice \n"
          "       end\n"
          "    } def\n"
          "}\n"
          "{\n"
          "    /setpagesize { pop pop } def\n"
          "} ifelse\n"
          "/duplex {\n"
          "    statusdict /setduplexmode known \n"
          "    { statusdict begin setduplexmode end } {pop} ifelse\n"
          "} def\n"
          "/tumble {\n"
          "    statusdict /settumble known\n"
          "   { statusdict begin settumble end } {pop} ifelse\n"
          "} def\n"
          "%% Turn the page around\n"
          "/turnpage {\n"
          "  90 rotate\n"
          "  0 pageheight neg translate\n"
          "} def\n",
          title,
          bb_page_width,
          bb_page_height,
          orientation_names[orientation]
          );
  
  fprintf(OUT,
          "%% User settings\n"
          "/pagewidth %d def\n"
          "/pageheight %d def\n"
          "pagewidth pageheight setpagesize\n"
          "/column_width %d def\n"
          
          "/bodyheight %d def\n"
          "/lmarg %d def\n"
          
          "/ytop %d def\n"
          "/do_separation_line %s def\n"
          "/do_landscape %s def\n"
          
          "/do_tumble %s def\n"
          "/do_duplex %s def\n",
          page_layout->page_width,
          page_layout->page_height,
          page_layout->column_width,
          page_layout->column_height,
          page_layout->left_margin,
          page_layout->page_height - bodytop,
          bool_name[page_layout->do_separation_line>0],
          bool_name[page_layout->do_landscape>0],
          bool_name[page_layout->do_tumble>0],
          bool_name[page_layout->do_duplex>0]
          );
  
  fprintf(OUT,
          "%% Procedures to translate position to first and second column\n"
          "/lw 20 def %% whatever\n"
          "/setnumcolumns {\n"
          "    /numcolumns exch def\n"
          "    /firstcolumn { /xpos lmarg def /ypos ytop def} def\n"
          "    /nextcolumn { \n"
          "      do_separation_line {\n"
          "          xpos column_width add gutter_width 2 div add %% x start\n"
          "           ytop lw add moveto              %% y start\n"
          "          0 bodyheight lw add neg rlineto 0 setlinewidth stroke\n"
          "      } if\n"
          "      /xpos xpos column_width add gutter_width add def \n"
          "      /ypos ytop def\n"
          "    } def\n"
          "} def\n"
          "\n"
          );
  
  fprintf(OUT,
          "%d setnumcolumns\n",
          page_layout->num_columns);
  fprintf(OUT,
          "/showline {\n"
          "    /y exch def\n"
          "    /s exch def\n"
          "    xpos y moveto \n"
          "    column_width 0 rlineto stroke\n"
          "    xpos y moveto /Helvetica findfont 20 scalefont setfont s show\n"
          "} def\n"
          );

  // The following definitions polute the global namespace. All such
  // definitions should start with paps_
  fprintf(OUT,
          "/paps_bop {  %% Beginning of page definitions\n"
          "    papsdict begin\n"
          "    gsave\n"
          "    do_landscape {turnpage} if \n"
          "    firstcolumn\n"
          "    end\n"
          "} def\n"
          "\n"
          "/paps_eop {  %% End of page cleanups\n"
          "    grestore    \n"
          "} def\n");

}

void print_postscript_trailer(FILE *OUT,
                             int num_pages)
{
  fprintf(OUT,
          "%%%%Pages: %d\n"
          "%%%%Trailer\n"
          "%%%%EOF\n",
          num_pages
          );
}

void eject_column(FILE *OUT,
                  page_layout_t *page_layout,
                  int column_idx)
{
  double x_pos, y_top, y_bot, total_gutter;

#if 0
  fprintf(stderr, "do_separation_line column_idx = %d %d\n", page_layout->do_separation_line, column_idx);
#endif
  if (!page_layout->do_separation_line)
    return;

  if (page_layout->pango_dir == PANGO_DIRECTION_RTL)
    column_idx = (page_layout->num_columns - column_idx);

  if (column_idx == 1)
    total_gutter = 1.0 * page_layout->gutter_width /2;
  else
    total_gutter = (column_idx + 1.5) * page_layout->gutter_width;
      
  x_pos = page_layout->left_margin
        + page_layout->column_width * column_idx
      + total_gutter;

  y_top = page_layout->page_height - page_layout->top_margin - page_layout->header_height - page_layout->header_sep / 2;
  y_bot = page_layout->bottom_margin - page_layout->footer_height;

  g_string_append_printf(ps_pages_string,
                         "%f %f moveto %f %f lineto 0 setlinewidth stroke\n",
                         x_pos, y_top,
                         x_pos, y_bot);
}

void eject_page(FILE *OUT)
{
  g_string_append_printf(ps_pages_string,
                         "paps_eop\n"
                         "showpage\n");
}

void start_page(FILE *OUT,
                int page_idx)
{
  g_string_append_printf(ps_pages_string,
                         "%%%%Page: %d %d\n"
                         "paps_bop\n",
                         page_idx, page_idx);
}

void
draw_line_to_page(FILE *OUT,
                  int column_idx,
                  int column_pos,
                  page_layout_t *page_layout,
                  PangoLayoutLine *line)
{
  /* Assume square aspect ratio for now */
  double y_pos = page_layout->page_height
               - page_layout->top_margin
               - page_layout->header_sep
               - column_pos / PANGO_SCALE * page_layout->pixel_to_pt;
  double x_pos = page_layout->left_margin
               + column_idx * (page_layout->column_width
                               + page_layout->gutter_width);
  gchar *ps_layout;

  PangoRectangle ink_rect, logical_rect;

  /* Do RTL column layout for RTL direction */
  if (page_layout->pango_dir == PANGO_DIRECTION_RTL)
    {
      x_pos = page_layout->left_margin
        + (page_layout->num_columns-1-column_idx)
        * (page_layout->column_width + page_layout->gutter_width);
    }
  
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);


  if (page_layout->pango_dir == PANGO_DIRECTION_RTL) {
      x_pos += page_layout->column_width  - logical_rect.width / (page_layout->pt_to_pixel * PANGO_SCALE);
  }

  ps_layout = paps_layout_line_to_postscript_strdup(paps,
                                                    x_pos, y_pos,
                                                    line);

  g_string_append(ps_pages_string,
                  ps_layout);
  g_free(ps_layout);
}

int
draw_page_header_line_to_page(FILE            *OUT,
                              gboolean         is_footer,
                              page_layout_t   *page_layout,
                              PangoContext    *ctx,
                              int              page)
{
  PangoLayout *layout = pango_layout_new(ctx);
  PangoLayoutLine *line;
  PangoRectangle ink_rect, logical_rect;
  /* Assume square aspect ratio for now */
  double x_pos, y_pos;
  gchar *ps_layout, *header, date[256];
  time_t t;
  struct tm tm;
  int height;
  gdouble line_pos;

  t = time(NULL);
  tm = *localtime(&t);
  strftime(date, 255, "%c", &tm);
  header = g_strdup_printf("<span font_desc=\"%s\">%s</span>\n"
                           "<span font_desc=\"%s\">%s</span>\n"
                           "<span font_desc=\"%s\">Page %d</span>",
                           page_layout->header_font_desc,
                           date,
                           page_layout->header_font_desc,
                           page_layout->filename,
                           page_layout->header_font_desc,
                           page);
  pango_layout_set_markup(layout, header, -1);
  g_free(header);

  /* output a left edge of header/footer */
  line = pango_layout_get_line(layout, 0);
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);
  x_pos = page_layout->left_margin;
  height = logical_rect.height / PANGO_SCALE * page_layout->pixel_to_pt/3.0;

  /* The header is placed right after the margin */
  if (is_footer)
    {
      y_pos = page_layout->bottom_margin;
      page_layout->footer_height = height;
    }
  else
    {
      y_pos = page_layout->page_height - page_layout->top_margin - height;
      page_layout->header_height = height;
    }
  ps_layout = paps_layout_line_to_postscript_strdup(paps,
                                                    x_pos, y_pos,
                                                    line);
  g_string_append(ps_pages_string,
                  ps_layout);
  g_free(ps_layout);

  /* output a center of header/footer */
  line = pango_layout_get_line(layout, 1);
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);
  x_pos = (page_layout->page_width - (logical_rect.width / PANGO_SCALE * page_layout->pixel_to_pt)) / 2;
  ps_layout = paps_layout_line_to_postscript_strdup(paps,
                                                    x_pos, y_pos,
                                                    line);
  g_string_append(ps_pages_string,
                  ps_layout);
  g_free(ps_layout);

  /* output a right edge of header/footer */
  line = pango_layout_get_line(layout, 2);
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);
  x_pos = page_layout->page_width - page_layout->right_margin - (logical_rect.width / PANGO_SCALE * page_layout->pixel_to_pt);
  ps_layout = paps_layout_line_to_postscript_strdup(paps,
                                                    x_pos, y_pos,
                                                    line);
  g_string_append(ps_pages_string,
                  ps_layout);
  g_free(ps_layout);

  g_object_unref(layout);

  /* header separator */
  line_pos = page_layout->page_height - page_layout->top_margin - page_layout->header_height - page_layout->header_sep / 2;
  g_string_append_printf(ps_pages_string,
                         "%d %f moveto %d %f lineto 0 setlinewidth stroke\n",
                         page_layout->left_margin, line_pos,
                         page_layout->page_width - page_layout->right_margin, line_pos);

  return logical_rect.height;
}

