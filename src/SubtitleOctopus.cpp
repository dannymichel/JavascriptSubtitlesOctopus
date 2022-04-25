/*
    SubtitleOctopus.js
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "../lib/libass/libass/ass.h"

#include "libass.cpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
// make IDE happy
#define emscripten_get_now() 0.0
#endif

int log_level = 3;

class ReusableBuffer {
public:
    ReusableBuffer(): buffer(NULL), size(-1), lessen_counter(0) {}

    ~ReusableBuffer() {
        free(buffer);
    }

    void clear() {
        free(buffer);
        buffer = NULL;
        size = -1;
        lessen_counter = 0;
    }

    void *take(int new_size, bool keep_content) {
        if (size >= new_size) {
            if (size >= 1.3 * new_size) {
                // big reduction request
                lessen_counter++;
            } else {
                lessen_counter = 0;
            }
            if (lessen_counter < 10) {
                // not reducing the buffer yet
                return buffer;
            }
        }

        void *newbuf;
        if (keep_content) {
            newbuf = realloc(buffer, new_size);
        } else {
            newbuf = malloc(new_size);
        }
        if (!newbuf) return NULL;

        if (!keep_content) free(buffer);
        buffer = newbuf;
        size = new_size;
        lessen_counter = 0;
        return buffer;
    }

    int capacity() const {
        return size;
    }

private:
    void *buffer;
    int size;
    int lessen_counter;
};

void msg_callback(int level, const char *fmt, va_list va, void *data) {
    if (level > log_level) // 6 for verbose
        return;

    const int ERR_LEVEL = 1;
    FILE* stream = level <= ERR_LEVEL ? stderr : stdout;

    fprintf(stream, "libass: ");
    vfprintf(stream, fmt, va);
    fprintf(stream, "\n");
}

const float MIN_UINT8_CAST = 0.9 / 255;
const float MAX_UINT8_CAST = 255.9 / 255;

#define CLAMP_UINT8(value) ((value > MIN_UINT8_CAST) ? ((value < MAX_UINT8_CAST) ? (int)(value * 255) : 255) : 0)

typedef struct RenderBlendResult {
public:
    int changed;
    double blend_time;
    int dest_x, dest_y, dest_width, dest_height;
    unsigned char* image;
} RenderBlendResult;

double libassjs_find_next_event_start(double tm) {
    if (!track || track->n_events == 0) return -1;

    ASS_Event *cur = track->events;
    long long now = (long long)(tm * 1000);
    long long closest = -1;

    for (int i = 0; i < track->n_events; i++, cur++) {
        long long start = cur->Start;
        if (start >= now && (start < closest || closest == -1)) {
            closest = start;
        }
    }

    return closest / 1000.0;
}

static int _is_move_tag_animated(char *begin, char *end) {
    int params[6];
    int count = 0, value = 0, num_digits = 0;
    for (; begin < end; begin++) {
        switch (*begin) {
            case ' ': // fallthrough
            case '\t':
                break;
            case ',':
                params[count] = value;
                count++;
                value = 0;
                num_digits = 0;
                break;
            default: {
                    int digit = *begin - '0';
                    if (digit < 0 || digit > 9) return 0; // invalid move
                    value = value * 10 + digit;
                    num_digits++;
                    break;
                }
        }
    }
    if (num_digits > 0) {
        params[count] = value;
        count++;
    }
    if (count < 4) return 0; // invalid move

    // move is animated if (x1,y1) != (x2,y2)
    return params[0] != params[2] || params[1] != params[3];
}

static int _is_animated_tag(char *begin, char *end) {
    // strip whitespaces around the tag
    while (begin < end && (*begin == ' ' || *begin == '\t')) begin++;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) end--;

    int length = end - begin;
    if (length < 3 || *begin != '\\') return 0; // too short to be animated or not a command

    switch (begin[1]) {
        case 'k': // fallthrough
        case 'K':
            // \kXX is karaoke
            return 1;
        case 't':
            // \t(...) is transition
            return length >= 4 && begin[2] == '(' && end[-1] == ')';
        case 'm':
            if (length >=7 && end[-1] == ')' && strcmp(begin, "\\move(") == 0) {
                return _is_move_tag_animated(begin + 6, end - 1);
            }
            break;
        case 'f':
            // \fad() or \fade() are fades
            return (length >= 7 && end[-1] == ')' &&
                (strcmp(begin, "\\fad(") == 0 || strcmp(begin, "\\fade(") == 0));
    }

    return 0;
}

static int _is_event_animated(ASS_Event *event) {
    // event is complex if it's animated in any way,
    // either by having non-empty Effect or
    // by having tags (enclosed in '{}' in Text)
    if (event->Effect && event->Effect[0] != '\0') return 1;

    int escaped = 0;
    char *tagStart = NULL;
    for (char *p = event->Text; *p != '\0'; p++) {
        switch (*p) {
            case '\\':
                escaped = !escaped;
                break;
            case '{':
                if (!escaped && tagStart == NULL) tagStart = p + 1;
                break;
            case '}':
                if (!escaped && tagStart != NULL) {
                    if (_is_animated_tag(tagStart, p)) return 1;
                    tagStart = NULL;
                }
                break;
            case ';':
                if (tagStart != NULL) {
                    if (_is_animated_tag(tagStart, p)) return 1;
                }
                tagStart = p + 1;
                break;
        }
    }

    return 0;
}

static void detect_animated_events() {
    ASS_Event *cur = track->events;
    int *animated = is_animated_events;
    for (int i = 0; i < track->n_events; i++, cur++, animated++) {
        *animated = _is_event_animated(cur);
    }
}

void libassjs_find_event_stop_times(double tm, double *eventFinish, double *emptyFinish, int *is_animated) {
    if (!track || track->n_events == 0) {
        *eventFinish = *emptyFinish = -1;
        return;
    }

    ASS_Event *cur = track->events;
    long long now = (long long)(tm * 1000);

    long long minFinish = -1, maxFinish = -1, minStart = -1;
    int current_animated = 0;

    for (int i = 0; i < track->n_events; i++, cur++) {
        long long start = cur->Start;
        long long finish = start + cur->Duration;
        if (start <= now) {
            if (finish > now) {
                if (finish < minFinish || minFinish == -1) {
                    minFinish = finish;
                }
                if (finish > maxFinish) {
                    maxFinish = finish;
                }
                if (!current_animated) current_animated = m_is_event_animated[i];
            }
        } else if (start < minStart || minStart == -1) {
            minStart = start;
        }
    }
    *is_animated = current_animated;

    if (minFinish != -1) {
        // some event is going on, so we need to re-draw either when it stops
        // or when some other event starts
        *eventFinish = ((minFinish < minStart) ? minFinish : minStart) / 1000.0;
    } else {
        // there's no current event, so no need to draw anything
        *eventFinish = -1;
    }

    if (minFinish == maxFinish && (minStart == -1 || minStart > maxFinish)) {
        // there's empty space after this event ends
        *emptyFinish = minStart / 1000.0;
    } else {
        // there's no empty space after eventFinish happens
        *emptyFinish = *eventFinish;
    }
}

class SubtitleOctopus {
public:
    ASS_Library* ass_library;
    ASS_Renderer* ass_renderer;
    ASS_Track* track;

    int canvas_w;
    int canvas_h;

    int status;

    SubtitleOctopus() {
        status = 0;
        ass_library = NULL;
        ass_renderer = NULL;
        track = NULL;
        canvas_w = 0;
        canvas_h = 0;
    }

    void setLogLevel(int level) {
        log_level = level;
    }

    void initLibrary(int frame_w, int frame_h) {
        ass_library = ass_library_init();
        if (!ass_library) {
            fprintf(stderr, "jso: ass_library_init failed!\n");
            exit(2);
        }

        ass_set_message_cb(ass_library, msg_callback, NULL);

        ass_renderer = ass_renderer_init(ass_library);
        if (!ass_renderer) {
            fprintf(stderr, "jso: ass_renderer_init failed!\n");
            exit(3);
        }

        resizeCanvas(frame_w, frame_h);

        reloadFonts();
        m_blend.clear();
        m_is_event_animated = NULL;
    }

    /* TRACK */
    void createTrack(char* subfile) {
        removeTrack();
        track = ass_read_file(ass_library, subfile, NULL);
        if (!track) {
            fprintf(stderr, "jso: Failed to start a track\n");
            exit(4);
        }

        free(m_is_event_animated);
        m_is_event_animated = (int*)malloc(sizeof(int) * track->n_events);
        if (m_is_event_animated == NULL) {
            printf("cannot parse animated events\n");
            exit(5);
        }
        detect_animated_events();
    }

    void createTrackMem(char *buf, unsigned long bufsize) {
        removeTrack();
        track = ass_read_memory(ass_library, buf, (size_t)bufsize, NULL);
        if (!track) {
            fprintf(stderr, "jso: Failed to start a track\n");
            exit(4);
        }
    }

    void removeTrack() {
        if (track != NULL) {
            ass_free_track(track);
            track = NULL;
        }
        free(m_is_event_animated);
        m_is_event_animated = NULL;
    }
    /* TRACK */

    /* CANVAS */
    void resizeCanvas(int frame_w, int frame_h) {
        ass_set_frame_size(ass_renderer, frame_w, frame_h);
        canvas_h = frame_h;
        canvas_w = frame_w;
    }
    ASS_Image* renderImage(double time, int* changed) {
        ASS_Image *img = ass_render_frame(ass_renderer, track, (int) (time * 1000), changed);
        return img;
    }
    /* CANVAS */

    void quitLibrary() {
        ass_free_track(track);
        ass_renderer_done(ass_renderer);
        ass_library_done(ass_library);
        m_blend.clear();
        free(m_is_event_animated);
        m_is_event_animated = NULL;
    }
    void reloadLibrary() {
        quitLibrary();

        initLibrary(canvas_w, canvas_h);
    }

    void reloadFonts() {
        ass_set_fonts(ass_renderer, "/assets/default.woff2", NULL, ASS_FONTPROVIDER_FONTCONFIG, "/assets/fonts.conf", 1);
    }

    void setMargin(int top, int bottom, int left, int right) {
        ass_set_margins(ass_renderer, top, bottom, left, right);
    }

    int getEventCount() const {
        return track->n_events;
    }

    int allocEvent() {
        return ass_alloc_event(track);
    }

    void removeEvent(int eid) {
        ass_free_event(track, eid);
    }

    int getStyleCount() const {
        return track->n_styles;
    }

    int getStyleByName(const char* name) const {
        for (int n = 0; n < track->n_styles; n++) {
            if (track->styles[n].Name && strcmp(track->styles[n].Name, name) == 0)
                return n;
        }
        return 0;
    }

    int allocStyle() {
        return ass_alloc_style(track);
    }

    void removeStyle(int sid) {
        ass_free_event(track, sid);
    }

    void removeAllEvents() {
        ass_flush_events(track);
    }

    void setMemoryLimits(int glyph_limit, int bitmap_cache_limit) {
        printf("jso: setting total libass memory limits to: glyph=%d MiB, bitmap cache=%d MiB\n",
            glyph_limit, bitmap_cache_limit);
        ass_set_cache_limits(ass_renderer, glyph_limit, bitmap_cache_limit);
    }

    RenderBlendResult* renderBlend(double tm, int force) {
        m_blendResult.blend_time = 0.0;
        m_blendResult.image = NULL;

        ASS_Image *img = ass_render_frame(ass_renderer, track, (int)(tm * 1000), &m_blendResult.changed);
        if (img == NULL || (m_blendResult.changed == 0 && !force)) {
            return &m_blendResult;
        }

        double start_blend_time = emscripten_get_now();

        // find bounding rect first
        int min_x = img->dst_x, min_y = img->dst_y;
        int max_x = img->dst_x + img->w - 1, max_y = img->dst_y + img->h - 1;
        ASS_Image *cur;
        for (cur = img->next; cur != NULL; cur = cur->next) {
            if (cur->w == 0 || cur->h == 0) continue; // skip empty images
            if (cur->dst_x < min_x) min_x = cur->dst_x;
            if (cur->dst_y < min_y) min_y = cur->dst_y;
            int right = cur->dst_x + cur->w - 1;
            int bottom = cur->dst_y + cur->h - 1;
            if (right > max_x) max_x = right;
            if (bottom > max_y) max_y = bottom;
        }

        int width = max_x - min_x + 1, height = max_y - min_y + 1;

        if (width == 0 || height == 0) {
            // all images are empty
            return &m_blendResult;
        }

        // make float buffer for blending
        float* buf = (float*)m_blend.take(sizeof(float) * width * height * 4, 0);
        if (buf == NULL) {
            fprintf(stderr, "jso: cannot allocate buffer for blending\n");
            return &m_blendResult;
        }
        memset(buf, 0, sizeof(float) * width * height * 4);

        // blend things in
        for (cur = img; cur != NULL; cur = cur->next) {
            int curw = cur->w, curh = cur->h;
            if (curw == 0 || curh == 0) continue; // skip empty images
            int a = (255 - (cur->color & 0xFF));
            if (a == 0) continue; // skip transparent images

            int curs = (cur->stride >= curw) ? cur->stride : curw;
            int curx = cur->dst_x - min_x, cury = cur->dst_y - min_y;

            unsigned char *bitmap = cur->bitmap;
            float normalized_a = a / 255.0;
            float r = ((cur->color >> 24) & 0xFF) / 255.0;
            float g = ((cur->color >> 16) & 0xFF) / 255.0;
            float b = ((cur->color >> 8) & 0xFF) / 255.0;

            int buf_line_coord = cury * width;
            for (int y = 0, bitmap_offset = 0; y < curh; y++, bitmap_offset += curs, buf_line_coord += width)
            {
                for (int x = 0; x < curw; x++)
                {
                    float pix_alpha = bitmap[bitmap_offset + x] * normalized_a / 255.0;
                    float inv_alpha = 1.0 - pix_alpha;

                    int buf_coord = (buf_line_coord + curx + x) << 2;
                    float *buf_r = buf + buf_coord;
                    float *buf_g = buf + buf_coord + 1;
                    float *buf_b = buf + buf_coord + 2;
                    float *buf_a = buf + buf_coord + 3;

                    // do the compositing, pre-multiply image RGB with alpha for current pixel
                    *buf_a = pix_alpha + *buf_a * inv_alpha;
                    *buf_r = r * pix_alpha + *buf_r * inv_alpha;
                    *buf_g = g * pix_alpha + *buf_g * inv_alpha;
                    *buf_b = b * pix_alpha + *buf_b * inv_alpha;
                }
            }
        }

        // now build the result;
        // NOTE: we use a "view" over [float,float,float,float] array of pixels,
        // so we _must_ go left-right top-bottom to not mangle the result
        unsigned int *result = (unsigned int*)buf;
        for (int y = 0, buf_line_coord = 0; y < height; y++, buf_line_coord += width) {
            for (int x = 0; x < width; x++) {
                unsigned int pixel = 0;
                int buf_coord = (buf_line_coord + x) << 2;
                float alpha = buf[buf_coord + 3];
                if (alpha > MIN_UINT8_CAST) {
                    // need to un-multiply the result
                    float value = buf[buf_coord] / alpha;
                    pixel |= CLAMP_UINT8(value); // R
                    value = buf[buf_coord + 1] / alpha;
                    pixel |= CLAMP_UINT8(value) << 8; // G
                    value = buf[buf_coord + 2] / alpha;
                    pixel |= CLAMP_UINT8(value) << 16; // B
                    pixel |= CLAMP_UINT8(alpha) << 24; // A
                }
                result[buf_line_coord + x] = pixel;
            }
        }

        // return the thing
        m_blendResult.dest_x = min_x;
        m_blendResult.dest_y = min_y;
        m_blendResult.dest_width = width;
        m_blendResult.dest_height = height;
        m_blendResult.blend_time = emscripten_get_now() - start_blend_time;
        m_blendResult.image = (unsigned char*)result;
        return &m_blendResult;
    }

private:
    ReusableBuffer m_blend;
    RenderBlendResult m_blendResult;
    int *m_is_event_animated;
};

int main(int argc, char *argv[]) { return 0; }

#ifdef __EMSCRIPTEN__
#include "./SubOctpInterface.cpp"
#endif
