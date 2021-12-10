#include <interface/Font.h>
#include <interface/Rect.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <regex.h>
#include <map>

#include "Font.h"

extern "C" {
#include <X11/Xlib.h>
#include <X11/Xatom.h>
}

struct FontEntry {
	BString xlfd;
	font_family family;
	font_style style;
	int default_size = 12;
};
static std::map<uint16_t, FontEntry*> sFonts;
static uint16_t sLastFontID = 1;

static Font
make_Font(uint16_t id, uint16_t pointSize)
{
	return (id & UINT16_MAX) | ((pointSize & UINT16_MAX) << 16);
}

static void
extract_Font(Font font, uint16_t& id, uint16_t& pointSize)
{
	id = (font & UINT16_MAX);
	pointSize = (pointSize >> 16) & UINT16_MAX;
}

static FontEntry*
lookup_font(int id)
{
	const auto& it = sFonts.find(id);
	if (it == sFonts.end())
		return NULL;
	return it->second;
}

static BString create_xlfd(font_family* family, font_style* style, uint16 face, uint32 flag);

void init_font()
{
	if (!sFonts.empty())
		return;

	font_family family;
	font_style style;
	const int max_family = count_font_families();
	for(int i = 0; i != max_family; i++) {
		get_font_family(i, &family);
		const int max_style = count_font_styles(family);
		for (int j = 0; j !=max_style; ++j) {
			uint32 flag;
			uint16 face;
			get_font_style(family, j, &style, &face, &flag);

			FontEntry* font = new FontEntry;
			memcpy(font->family, family, sizeof(family));
			memcpy(font->style, style, sizeof(style));
			font->xlfd = create_xlfd(&family, &style, face, flag);
			sFonts.insert({sLastFontID++, font});
		}
	}

	// Special case: "fixed" and "cursor".
	FontEntry* font = new FontEntry;
	be_fixed_font->GetFamilyAndStyle(&family, &style);
	memcpy(font->family, family, sizeof(family));
	memcpy(font->style, style, sizeof(style));
	font->xlfd = "fixed";
	font->default_size = 10;
	sFonts.insert({sLastFontID++, font});

	font = new FontEntry(*font);
	font->xlfd = "cursor";
	sFonts.insert({sLastFontID++, font});
}

void finalize_font()
{
	for (const auto& item : sFonts)
		delete item.second;
	sFonts.clear();
}

BFont
bfont_from_font(Font fid)
{
	uint16_t id, pointSize;
	extract_Font(fid, id, pointSize);

	BFont font;
	FontEntry* fontId = lookup_font(id);
	if (!fontId)
		return font;
	font.SetFamilyAndStyle(fontId->family, fontId->style);
	if (pointSize)
		font.SetSize(pointSize);
	else if (fontId->default_size)
		font.SetSize(fontId->default_size);
	return font;
}

static char
get_slant(font_style* style, uint16 face)
{
	if ((face & B_ITALIC_FACE) || strstr(*style, "Italic") == NULL)
		return 'r';
	return 'i';
}

static char
get_spacing(uint32 flag)
{
	if (flag & B_IS_FIXED)
		return 'm';
	return 'p';
}

static const char*
get_weight(font_style* style, uint16 face)
{
	static const char* medium = "medium";
	static const char* bold = "bold";
	if ((face & B_BOLD_FACE) || strstr(*style, "Bold") == NULL)
		return bold;
	return medium;
}

static const char*
get_encoding(font_family* family)
{
	static const char* iso8859_1 = "iso8859-1";
	static const char* hankaku = "jisx0201.1976-0";
	static const char* kanji = "jisx0208.1983-0";

	static const char* testchar = "\xEF\xBD\xB1\xE4\xBA\x9C";

	BFont font;
	font.SetFamilyAndFace(*family, B_REGULAR_FACE);
	switch (font.Encoding()) {
	case B_UNICODE_UTF8:
		// FIXME: just say it's Latin-1 for now
	case B_ISO_8859_1:
		return iso8859_1;
	default: {
		static bool check[2];
		font.GetHasGlyphs(testchar, 2, check);
		if (check[1])
			return kanji;
		if (check[0])
			return hankaku;
		// presume Latin?
		return iso8859_1;
	}
	}
}

static BString
create_xlfd(font_family* family, font_style* style, uint16 face, uint32 flag)
{
	BString string;
	string.SetToFormat("-TTFont-%s-%s-%c-normal--0-0-0-0-%c-0-%s",
		*family, get_weight(style, face), get_slant(style, face), get_spacing(flag), get_encoding(family));
	return string;
}

static int
count_wc(const char* pattern)
{
	int count = 0;
	for (const char* i = pattern; *i != '\0' ; i++) {
		if (*i == '*')
			++count;
	}
	return count;
}

static char*
create_regex_pattern_string(const char* pattern, uint16& size)
{
	const int end = strlen(pattern);
	char* regpattern = (char*)malloc(end + count_wc(pattern) + 1);

	int i = 0;
	int field = 0;
	for (const char* c = pattern; c != (pattern + end); c++) {
		switch (*c) {
		case '-':
			regpattern[i++] = *c;
			field++;

			if (field == 7 || field == 8) {
				// Specify '0' for sizes, and pass back the original size (as points).
				const char* start = c;
				regpattern[i++] = '0';
				while (c[1] != '-')
					c++;

				const int parseSize = strtol(start + 1, NULL, 10);
				if (parseSize > 0) {
					if (field == 6)
						size = parseSize * 0.75f;
					else if (field == 7)
						size = parseSize / 10;
				}
			}
		break;
		case '*':
			regpattern[i++] = '.';
			regpattern[i++] = '*';
		break;
		case '?':
			regpattern[i++] = '.';
			// fall through
		default:
			regpattern[i++] = *c;
		}
	}
	regpattern[i++] = '\0';
	return regpattern;
}

static void
compile_font_pattern(const char* pattern, regex_t& regex, uint16& ptSize)
{
	if ((pattern == NULL) || (strcmp(pattern, "") == 0))
		regcomp(&regex, ".*", REG_NOSUB);
	else
		regcomp(&regex, create_regex_pattern_string(pattern, ptSize), REG_NOSUB);
}

extern "C" char**
XListFontsWithInfo(Display* display,
	const char*	pattern, int maxNames, int* count, XFontStruct** info_return)
{
	*count = 0;

	regex_t regex;
	uint16 ptSize;
	compile_font_pattern(pattern, regex, ptSize);

	char** nameList = (char**) malloc(maxNames * sizeof(char*) + 1);
	for (const auto& font : sFonts) {
		if (regexec(&regex, font.second->xlfd.String(), 0, 0, 0) == 0) {
			int index = (*count)++;
			nameList[index] = strdup(font.second->xlfd.String());
			if (info_return)
				info_return[index] = XQueryFont(display, font.first);
		}

		if ((*count) == maxNames)
			break;
	}
	regfree(&regex);

	nameList[*count] = 0;
	return nameList;
}

extern "C" Font
XLoadFont(Display *dpy, const char *name)
{
	regex_t regex;
	uint16 ptSize;
	compile_font_pattern(name, regex, ptSize);

	int id = 0;
	for (const auto& font : sFonts) {
		if (regexec(&regex, font.second->xlfd.String(), 0, 0, 0) == 0) {
			id = font.first;
			break;
		}
	}
	regfree(&regex);

	if (id != 0)
		return make_Font(id, ptSize);
	return 0;
}

extern "C" char**
XListFonts(Display *dpy, const char *pattern, int maxNames, int *count)
{
	return XListFontsWithInfo(dpy, pattern, maxNames, count, NULL);
}

extern "C" int
XFreeFontNames(char** list)
{
	if (list) {
		int i = 0;
		while (list[i] != NULL) {
			free(list[i]);
			i++;
		}
	}
	free(list);
	return 0;
}

extern "C" XFontStruct*
XQueryFont(Display *display, Font id)
{
	FontEntry* ident = lookup_font(id);
	if (!ident)
		return NULL;

	BFont bfont = bfont_from_font(id);

	XFontStruct* font = (XFontStruct*)calloc(1, sizeof(XFontStruct));
	font->fid = id;
	font->direction = (bfont.Direction() == B_FONT_LEFT_TO_RIGHT)
		? FontLeftToRight : FontRightToLeft;

	font_height height;
	bfont.GetHeight(&height);
	font->ascent = font->max_bounds.ascent = height.ascent;
	font->descent = font->max_bounds.descent = height.descent;

	// TODO: Improve!
	font->max_bounds.width = bfont.StringWidth("@");

	// TODO: Fill in more!
	return font;
}

extern "C" XFontStruct*
XLoadQueryFont(Display *display, const char *name)
{
	return XQueryFont(display, XLoadFont(display, name));
}

extern "C" int
XFreeFont(Display *dpy, XFontStruct *fs)
{
	free(fs);
	return Success;
}

extern "C" Bool
XGetFontProperty(XFontStruct* font_struct, Atom atom, Atom* value_return)
{
	uint16_t id, pointSize;
	extract_Font(font_struct->fid, id, pointSize);
	if (atom == XA_FONT) {
		for (const auto& font : sFonts) {
		   if (font.first != id)
			   continue;
		   *value_return = XInternAtom(NULL, font.second->xlfd.String(), False);
		   return True;
		}
	}
	return False;
}

extern "C" int
XTextWidth(XFontStruct* font_struct, const char *string, int count)
{
	BFont bfont = bfont_from_font(font_struct->fid);
	return bfont.StringWidth(string, count);
}

extern "C" int
XTextWidth16(XFontStruct *font_struct, const XChar2b *string, int count)
{
	return XTextWidth(font_struct, (const char*)string, count);
}

extern "C" int
XTextExtents(XFontStruct* font_struct, const char* string, int nchars,
	int* direction_return, int* font_ascent_return, int* font_descent_return, XCharStruct* overall_return)
{
	const BFont bfont = bfont_from_font(font_struct->fid);
	BString copy(string, nchars);
	const char* strings[] = {copy.String()};
	BRect boundingBoxes[1];
	bfont.GetBoundingBoxesForStrings(strings, 1, B_SCREEN_METRIC, NULL, boundingBoxes);

	if (direction_return)
		*direction_return = font_struct->direction;
	if (font_ascent_return)
		*font_ascent_return = font_struct->ascent;
	if (font_descent_return)
		*font_descent_return = font_struct->descent;

	memset(overall_return, 0, sizeof(XCharStruct));
	overall_return->ascent = boundingBoxes[0].top;
	overall_return->descent = boundingBoxes[0].bottom;
	overall_return->width = boundingBoxes[0].IntegerWidth();
	return Success;
}
