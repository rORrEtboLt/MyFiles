#include "pdf.h"

#include <SDL3/SDL.h>

#define A4_WIDTH_PT 595.28

typedef struct {
    int width, height, components;
} JpegInfo;

/* Reads dimensions and colour components from the first SOF marker. */
static bool jpeg_info(const Uint8 *d, size_t len, JpegInfo *out)
{
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t i = 2;
    while (i + 9 < len) {
        if (d[i] != 0xFF) return false;
        Uint8 marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }          /* fill byte */
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
        size_t seg = (size_t)d[i + 2] << 8 | d[i + 3];
        /* SOF0..SOF15 except DHT (C4), JPG (C8), DAC (CC) */
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            out->height = d[i + 5] << 8 | d[i + 6];
            out->width = d[i + 7] << 8 | d[i + 8];
            out->components = d[i + 9];
            return out->width > 0 && out->height > 0;
        }
        if (marker == 0xDA) return false; /* start of scan before any SOF */
        i += 2 + seg;
    }
    return false;
}

int pdf_from_jpegs(const char *const *jpeg_paths, int count, const char *out_path)
{
    if (count <= 0) return 0;
    char tmp_path[1200];
    SDL_snprintf(tmp_path, sizeof tmp_path, "%s.tmp", out_path);
    SDL_IOStream *io = SDL_IOFromFile(tmp_path, "wb");
    if (!io) return 0;

    /* Objects: 1 catalog, 2 page tree, then per page: page, contents, image. */
    int max_objs = 2 + count * 3;
    Sint64 *offsets = SDL_calloc((size_t)max_objs + 1, sizeof *offsets);
    int *page_objs = SDL_calloc((size_t)count, sizeof *page_objs);
    int next_obj = 3, pages = 0;
    bool ok = true;

    SDL_IOprintf(io, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
    for (int p = 0; p < count && ok; p++) {
        size_t len = 0;
        Uint8 *jpeg = SDL_LoadFile(jpeg_paths[p], &len);
        JpegInfo info;
        if (!jpeg || !jpeg_info(jpeg, len, &info)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "pdf: skipping %s (not a readable JPEG)", jpeg_paths[p]);
            SDL_free(jpeg);
            continue;
        }
        const char *cs = info.components == 1 ? "/DeviceGray" : info.components == 4 ? "/DeviceCMYK" : "/DeviceRGB";
        double w = A4_WIDTH_PT, h = A4_WIDTH_PT * info.height / info.width;
        int page_obj = next_obj++, content_obj = next_obj++, image_obj = next_obj++;

        offsets[page_obj] = SDL_TellIO(io);
        SDL_IOprintf(io, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
                         "/Resources << /XObject << /Im%d %d 0 R >> >> /Contents %d 0 R >>\nendobj\n",
                     page_obj, w, h, p, image_obj, content_obj);

        char content[128];
        int clen = SDL_snprintf(content, sizeof content, "q %.2f 0 0 %.2f 0 0 cm /Im%d Do Q\n", w, h, p);
        offsets[content_obj] = SDL_TellIO(io);
        SDL_IOprintf(io, "%d 0 obj\n<< /Length %d >>\nstream\n%s\nendstream\nendobj\n", content_obj, clen, content);

        offsets[image_obj] = SDL_TellIO(io);
        SDL_IOprintf(io, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace %s "
                         "/BitsPerComponent 8 /Filter /DCTDecode%s /Length %zu >>\nstream\n",
                     image_obj, info.width, info.height, cs,
                     info.components == 4 ? " /Decode [1 0 1 0 1 0 1 0]" : "", len);
        ok = SDL_WriteIO(io, jpeg, len) == len;
        SDL_IOprintf(io, "\nendstream\nendobj\n");
        SDL_free(jpeg);
        page_objs[pages++] = page_obj;
    }

    if (ok && pages > 0) {
        offsets[1] = SDL_TellIO(io);
        SDL_IOprintf(io, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
        offsets[2] = SDL_TellIO(io);
        SDL_IOprintf(io, "2 0 obj\n<< /Type /Pages /Count %d /Kids [", pages);
        for (int i = 0; i < pages; i++) SDL_IOprintf(io, "%d 0 R ", page_objs[i]);
        SDL_IOprintf(io, "] >>\nendobj\n");

        /* Skipped pages leave unused object numbers; list them as free entries. */
        Sint64 xref = SDL_TellIO(io);
        SDL_IOprintf(io, "xref\n0 %d\n0000000000 65535 f \n", next_obj);
        for (int i = 1; i < next_obj; i++) {
            if (offsets[i]) SDL_IOprintf(io, "%010lld 00000 n \n", (long long)offsets[i]);
            else SDL_IOprintf(io, "0000000000 65535 f \n");
        }
        SDL_IOprintf(io, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%lld\n%%%%EOF\n", next_obj, (long long)xref);
    }
    ok = SDL_CloseIO(io) && ok && pages > 0;
    SDL_free(offsets);
    SDL_free(page_objs);

    if (ok) ok = SDL_RenamePath(tmp_path, out_path);
    if (!ok) SDL_RemovePath(tmp_path);
    return ok ? pages : 0;
}
