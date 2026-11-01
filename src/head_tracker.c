/* Head tracking, baked into the relay: webcam -> neural network -> where your head points.
   #included by hotas_relay.c. It runs on its own two threads, only while it's wanted, and
   hands back everything when it stops: camera, memory, threads.

   The method is opentrack 2026.1.0's, the version the feel was tuned on, ported to C:
   Michael Welter's NeuralNet tracker (ISC) and Stanislaw Halik's Accela filter and pipeline
   (ISC). Same models, same image steps, same camera route, same filter settings, about the
   same 250 updates a second. See THIRD_PARTY_LICENSES.md.

   Two things stay outside: Microsoft's onnxruntime.dll, which runs the network, and the
   model files. They live in the head_tracker folder beside the relay and are only loaded
   when tracking starts. The camera goes through DirectShow, like opentrack's, so anything
   Windows offers as a camera works, virtual ones like OBS and NVIDIA Broadcast included. */
#include <stdlib.h>
#include <stddef.h>
#define COBJMACROS
#include <initguid.h>
#include <dshow.h>
#include <qedit.h>
#include "onnxruntime/onnxruntime_c_api.h"

#define HT_FOLDER "head_tracker"
#define HT_LOCALIZER_MODEL "head-localizer.onnx"
#define HT_POSE_MODEL "head-pose-0.4-big-int8.onnx"
#define HT_THREADS 2       /* for the network: opentrack's setting, and the one flown with */
#define HT_FOV_DEG 56.0    /* opentrack's default diagonal field of view */
#define HT_LEVELS 3 /* the full picture, and up to two halvings: the tracker reads the last one */
#define HT_LOC_W 288
#define HT_LOC_H 224
#define HT_FILTER_MS 4

typedef struct { float x, y, w, h; } ht_rect;
typedef struct { double w, x, y, z; } ht_quat;

/* ---------------------------------------------------------------- images */

typedef struct { int w, h; unsigned char *p; } ht_image;

static int ht_image_size(ht_image *im, int w, int h) {
    if (im->p && im->w == w && im->h == h) return 1;
    free(im->p);
    im->p = (unsigned char *)malloc((size_t)w * (size_t)h);
    im->w = w;
    im->h = h;
    return im->p != NULL;
}

/* cv::pyrDown: a 5-tap blur, every other pixel, mirrored at the edges. */
static int ht_mirror(int i, int n) {
    if (n == 1) return 0;
    while (i < 0 || i >= n) i = i < 0 ? -i : 2 * n - 2 - i;
    return i;
}

static void ht_pyr_down(const ht_image *s, ht_image *d) {
    static int *row[5];
    static int rowW = 0;
    const int dw = (s->w + 1) / 2, dh = (s->h + 1) / 2;
    if (!ht_image_size(d, dw, dh)) return;
    if (rowW < dw) {
        for (int i = 0; i < 5; i++) { free(row[i]); row[i] = (int *)malloc(sizeof(int) * (size_t)dw); }
        rowW = dw;
    }
    for (int y = 0; y < dh; y++) {
        for (int k = 0; k < 5; k++) {
            const unsigned char *r = s->p + (size_t)ht_mirror(2 * y + k - 2, s->h) * (size_t)s->w;
            for (int x = 0; x < dw; x++) {
                int c = 2 * x;
                row[k][x] = 6 * r[c] + 4 * (r[ht_mirror(c - 1, s->w)] + r[ht_mirror(c + 1, s->w)]) + r[ht_mirror(c - 2, s->w)] + r[ht_mirror(c + 2, s->w)];
            }
        }
        for (int x = 0; x < dw; x++)
            d->p[(size_t)y * (size_t)dw + (size_t)x] = (unsigned char)((6 * row[2][x] + 4 * (row[1][x] + row[3][x]) + row[0][x] + row[4][x] + 128) >> 8);
    }
}

/* cv::resize with INTER_AREA. Shrinking averages over each destination pixel's share of the
   source. Growing, which happens when a face is small, uses OpenCV's own area-mode weights. */
typedef struct { int di, si; float a; } ht_area;

static int ht_area_tab(int ssize, int dsize, double scale, ht_area *tab) {
    int k = 0;
    for (int dx = 0; dx < dsize; dx++) {
        double f1 = dx * scale, f2 = f1 + scale, cell = scale < ssize - f1 ? scale : ssize - f1;
        int s1 = (int)ceil(f1), s2 = (int)floor(f2);
        if (s2 > ssize - 1) s2 = ssize - 1;
        if (s1 > s2) s1 = s2;
        if (s1 - f1 > 1e-3) tab[k++] = (ht_area){dx, s1 - 1, (float)((s1 - f1) / cell)};
        for (int sx = s1; sx < s2; sx++) tab[k++] = (ht_area){dx, sx, (float)(1.0 / cell)};
        if (f2 - s2 > 1e-3) {
            double part = f2 - s2 < 1. ? f2 - s2 : 1.;
            if (part > cell) part = cell;
            tab[k++] = (ht_area){dx, s2, (float)(part / cell)};
        }
    }
    return k;
}

static void ht_resize_area(const unsigned char *s, int sw, int sh, unsigned char *d, int dw, int dh) {
    const double sx = (double)sw / dw, sy = (double)sh / dh;
    if (sx >= 1 && sy >= 1) {
        ht_area *xt = (ht_area *)malloc(sizeof(ht_area) * ((size_t)sw + (size_t)dw) * 2);
        ht_area *yt = (ht_area *)malloc(sizeof(ht_area) * ((size_t)sh + (size_t)dh) * 2);
        float *line = (float *)malloc(sizeof(float) * (size_t)dw), *sum = (float *)calloc((size_t)dw, sizeof(float));
        if (xt && yt && line && sum) {
            int xn = ht_area_tab(sw, dw, sx, xt), yn = ht_area_tab(sh, dh, sy, yt);
            for (int j = 0; j < yn; j++) {
                const unsigned char *r = s + (size_t)yt[j].si * (size_t)sw;
                memset(line, 0, sizeof(float) * (size_t)dw);
                for (int k = 0; k < xn; k++) line[xt[k].di] += r[xt[k].si] * xt[k].a;
                for (int x = 0; x < dw; x++) sum[x] += line[x] * yt[j].a;
                if (j + 1 == yn || yt[j + 1].di != yt[j].di) {
                    unsigned char *o = d + (size_t)yt[j].di * (size_t)dw;
                    for (int x = 0; x < dw; x++) {
                        float v = sum[x] + 0.5f;
                        o[x] = v <= 0 ? 0 : v >= 255 ? 255 : (unsigned char)v;
                        sum[x] = 0;
                    }
                }
            }
        }
        free(xt); free(yt); free(line); free(sum);
        return;
    }
    const double ix = 1.0 / sx, iy = 1.0 / sy;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)floor(y * sy);
        float fy = (float)((y + 1) - (y0 + 1) * iy);
        fy = fy <= 0 ? 0.f : fy - (float)floor(fy);
        if (y0 >= sh - 1) { y0 = sh - 1; fy = 0; }
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)floor(x * sx);
            float fx = (float)((x + 1) - (x0 + 1) * ix);
            fx = fx <= 0 ? 0.f : fx - (float)floor(fx);
            if (x0 >= sw - 1) { x0 = sw - 1; fx = 0; }
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float top = s[(size_t)y0 * sw + x0] * (1 - fx) + s[(size_t)y0 * sw + x1] * fx;
            float bot = s[(size_t)y1 * sw + x0] * (1 - fx) + s[(size_t)y1 * sw + x1] * fx;
            d[(size_t)y * dw + x] = (unsigned char)(top * (1 - fy) + bot * fy + 0.5f);
        }
    }
}

/* cv::getRectSubPix: a square cut at a fractional center, with edges repeated. */
static void ht_cut_patch(const ht_image *s, int size, float cx, float cy, unsigned char *out) {
    const float ox = cx - (size - 1) * 0.5f, oy = cy - (size - 1) * 0.5f;
    const int ix = (int)floor(ox), iy = (int)floor(oy);
    const float a = ox - ix, b = oy - iy;
    for (int y = 0; y < size; y++) {
        int y0 = iy + y, y1 = y0 + 1;
        y0 = y0 < 0 ? 0 : y0 >= s->h ? s->h - 1 : y0;
        y1 = y1 < 0 ? 0 : y1 >= s->h ? s->h - 1 : y1;
        const unsigned char *r0 = s->p + (size_t)y0 * s->w, *r1 = s->p + (size_t)y1 * s->w;
        for (int x = 0; x < size; x++) {
            int x0 = ix + x, x1 = x0 + 1;
            x0 = x0 < 0 ? 0 : x0 >= s->w ? s->w - 1 : x0;
            x1 = x1 < 0 ? 0 : x1 >= s->w ? s->w - 1 : x1;
            out[(size_t)y * size + x] = (unsigned char)(r0[x0] * (1 - a) * (1 - b) + r0[x1] * a * (1 - b) + r1[x0] * (1 - a) * b + r1[x1] * a * b + 0.5f);
        }
    }
}

/* ---------------------------------------------------------------- the two networks */

static const OrtApi *g_ort = NULL;

typedef struct {
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtMemoryInfo *mem;
    OrtSession *loc, *pose;
    char *poseInput;
    int poseW, poseH, poseVersion;
    float locIn[HT_LOC_H * HT_LOC_W];
    unsigned char locImg[HT_LOC_H * HT_LOC_W];
    float *poseIn;
    unsigned char *poseImg;
    double locMs, poseMs;
} ht_nets;

static int ht_ok(OrtStatus *st, const char *what) {
    if (!st) return 1;
    log_line("head tracking: %s: %s", what, g_ort->GetErrorMessage(st));
    g_ort->ReleaseStatus(st);
    return 0;
}

static void ht_folder(char *out, size_t len, const char *file) {
    char dir[MAX_PATH];
    get_exe_dir(dir, sizeof(dir));
    snprintf(out, len, "%s" HT_FOLDER "\\%s", dir, file);
}

/* Are the engine and models beside the relay? */
static int ht_files_present(void) {
    static const char *const files[] = {"onnxruntime.dll", HT_LOCALIZER_MODEL, HT_POSE_MODEL};
    char path[MAX_PATH + 64];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        ht_folder(path, sizeof(path), files[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    }
    return 1;
}

static int ht_load_session(ht_nets *n, const char *file, OrtSession **out) {
    char path[MAX_PATH + 64];
    wchar_t wide[MAX_PATH + 64];
    ht_folder(path, sizeof(path), file);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH + 64);
    return ht_ok(g_ort->CreateSession(n->env, wide, n->opts, out), file);
}

static void ht_nets_close(ht_nets *n) {
    if (n->pose) g_ort->ReleaseSession(n->pose);
    if (n->loc) g_ort->ReleaseSession(n->loc);
    if (n->mem) g_ort->ReleaseMemoryInfo(n->mem);
    if (n->opts) g_ort->ReleaseSessionOptions(n->opts);
    if (n->env) g_ort->ReleaseEnv(n->env);
    free(n->poseIn);
    free(n->poseImg);
    free(n->poseInput);
    free(n);
}

static ht_nets *ht_nets_open(void) {
    if (!g_ort) {
        /* the engine stays loaded once it's been used: unloading it isn't safe, and its memory is what the sessions held */
        char path[MAX_PATH + 64];
        ht_folder(path, sizeof(path), "onnxruntime.dll");
        HMODULE dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) { log_line("head tracking: couldn't load %s (error %lu)", path, GetLastError()); return NULL; }
        typedef const OrtApiBase *(ORT_API_CALL *base_fn)(void);
        base_fn base = (base_fn)(void *)GetProcAddress(dll, "OrtGetApiBase");
        g_ort = base ? base()->GetApi(ORT_API_VERSION) : NULL;
        if (!g_ort) { log_line("head tracking: onnxruntime.dll is too old for this relay"); return NULL; }
    }
    ht_nets *n = (ht_nets *)calloc(1, sizeof(ht_nets));
    if (!n) return NULL;
    OrtAllocator *alloc = NULL;
    OrtTypeInfo *ti = NULL;
    const OrtTensorTypeAndShapeInfo *tsi = NULL;
    OrtModelMetadata *meta = NULL;
    char *name = NULL;
    int64_t dims[4] = {0};
    int64_t version = 0;
    if (!ht_ok(g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "head", &n->env), "env") ||
        !ht_ok(g_ort->CreateSessionOptions(&n->opts), "options") ||
        !ht_ok(g_ort->SetIntraOpNumThreads(n->opts, HT_THREADS), "options") ||
        !ht_ok(g_ort->SetInterOpNumThreads(n->opts, 1), "options") ||
        !ht_ok(g_ort->AddSessionConfigEntry(n->opts, "session.intra_op.allow_spinning", "0"), "options") ||
        !ht_ok(g_ort->AddSessionConfigEntry(n->opts, "session.inter_op.allow_spinning", "0"), "options") ||
        !ht_ok(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &n->mem), "memory") ||
        !ht_load_session(n, HT_LOCALIZER_MODEL, &n->loc) || !ht_load_session(n, HT_POSE_MODEL, &n->pose) ||
        !ht_ok(g_ort->GetAllocatorWithDefaultOptions(&alloc), "allocator") ||
        !ht_ok(g_ort->SessionGetInputName(n->pose, 0, alloc, &name), "model input") ||
        !ht_ok(g_ort->SessionGetInputTypeInfo(n->pose, 0, &ti), "model input") ||
        !ht_ok(g_ort->CastTypeInfoToTensorInfo(ti, &tsi), "model input") ||
        !ht_ok(g_ort->GetDimensions(tsi, dims, 4), "model input") ||
        !ht_ok(g_ort->SessionGetModelMetadata(n->pose, &meta), "model") ||
        !ht_ok(g_ort->ModelMetadataGetVersion(meta, &version), "model")) {
        if (ti) g_ort->ReleaseTypeInfo(ti);
        if (meta) g_ort->ReleaseModelMetadata(meta);
        ht_nets_close(n);
        return NULL;
    }
    n->poseInput = _strdup(name);
    ht_ok(g_ort->AllocatorFree(alloc, name), "allocator");
    g_ort->ReleaseTypeInfo(ti);
    g_ort->ReleaseModelMetadata(meta);
    n->poseH = (int)dims[2];
    n->poseW = (int)dims[3];
    n->poseVersion = version <= 0 || version > 4 ? 1 : (int)version;
    n->poseIn = (float *)malloc(sizeof(float) * (size_t)n->poseW * (size_t)n->poseH);
    n->poseImg = (unsigned char *)malloc((size_t)n->poseW * (size_t)n->poseH);
    if (!n->poseIn || !n->poseImg || !n->poseInput) { ht_nets_close(n); return NULL; }
    return n;
}

static int ht_run(ht_nets *n, OrtSession *s, const char *inName, float *in, int w, int h, int outCount, const char *const *outNames, float *const *outBufs, const int *outLens, double *ms) {
    int64_t inShape[4] = {1, 1, h, w};
    OrtValue *inVal = NULL, *outVal[4] = {0};
    int ok = 0;
    LARGE_INTEGER t0, t1, fq;
    if (ht_ok(g_ort->CreateTensorWithDataAsOrtValue(n->mem, in, sizeof(float) * (size_t)w * (size_t)h, inShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal), "input")) {
        ok = 1;
        for (int i = 0; i < outCount; i++) {
            int64_t shape[2] = {1, outLens[i]};
            ok = ok && ht_ok(g_ort->CreateTensorWithDataAsOrtValue(n->mem, outBufs[i], sizeof(float) * (size_t)outLens[i], shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outVal[i]), "output");
        }
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        ok = ok && ht_ok(g_ort->Run(s, NULL, &inName, (const OrtValue *const *)&inVal, 1, outNames, (size_t)outCount, outVal), "network");
        QueryPerformanceCounter(&t1);
        *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    }
    for (int i = 0; i < outCount; i++) if (outVal[i]) g_ort->ReleaseValue(outVal[i]);
    if (inVal) g_ort->ReleaseValue(inVal);
    return ok;
}

/* Finds the head in the whole picture. Returns the score, 0..1. */
static float ht_localize(ht_nets *n, const ht_image *im, ht_rect *roi) {
    float r[5] = {0};
    float *bufs[1] = {r};
    static const char *const names[1] = {"logit_box"};
    static const int lens[1] = {5};
    ht_resize_area(im->p, im->w, im->h, n->locImg, HT_LOC_W, HT_LOC_H);
    for (int i = 0; i < HT_LOC_W * HT_LOC_H; i++) n->locIn[i] = n->locImg[i] / 255.0f - 0.5f;
    if (!ht_run(n, n->loc, "x", n->locIn, HT_LOC_W, HT_LOC_H, 1, names, bufs, lens, &n->locMs)) return 0.f;
    float x0 = 0.5f * (r[1] + 1) * im->w, y0 = 0.5f * (r[2] + 1) * im->h;
    float x1 = 0.5f * (r[3] + 1) * im->w, y1 = 0.5f * (r[4] + 1) * im->h;
    *roi = (ht_rect){x0, y0, x1 - x0, y1 - y0};
    return 1.f / (1.f + expf(-r[0]));
}

/* The brightest 10% of the picture sets the scale, so a dark room still reads. */
static void ht_normalize_brightness(const unsigned char *p, int count, float *out) {
    int hist[256] = {0}, level = 0, acc = 0;
    const float pct = 90;
    for (int i = 0; i < count; i++) hist[p[i]]++;
    const int quantile = (int)(count * pct * 0.01f);
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc > quantile) { level = i; break; }
    }
    const double alpha = level < 127 ? (pct / 100.f * 0.5f / (level > 5 ? level : 5)) : 1. / 255;
    for (int i = 0; i < count; i++) out[i] = (float)(p[i] * alpha - 0.5);
}

typedef struct { float cx, cy, size; float qx, qy, qz, qw; ht_rect box; } ht_face;

/* Reads the head's turn from a cut around it. */
static int ht_estimate_pose(ht_nets *n, const ht_image *im, const ht_rect *box, ht_face *f) {
    const int bx = (int)lrintf(box->x), by = (int)lrintf(box->y), bw = (int)lrintf(box->w), bh = (int)lrintf(box->h);
    const int patch = (int)((bw > bh ? bw : bh) * 1.05);
    if (patch <= 0) return 0;
    float cx = bx + 0.5f * bw, cy = by + 0.5f * bh;
    cx = cx < 0 ? 0 : cx > im->w ? (float)im->w : cx;
    cy = cy < 0 ? 0 : cy > im->h ? (float)im->h : cy;
    unsigned char *cut = (unsigned char *)malloc((size_t)patch * (size_t)patch);
    if (!cut) return 0;
    ht_cut_patch(im, patch, cx, cy, cut);
    ht_resize_area(cut, patch, patch, n->poseImg, n->poseW, n->poseH);
    free(cut);
    ht_normalize_brightness(n->poseImg, n->poseW * n->poseH, n->poseIn);
    float coord[3] = {0}, quat[4] = {0}, outBox[4] = {0};
    float *bufs[3] = {coord, quat, outBox};
    static const char *const names[3] = {"pos_size", "quat", "box"};
    static const int lens[3] = {3, 4, 4};
    if (!ht_run(n, n->pose, n->poseInput, n->poseIn, n->poseW, n->poseH, 3, names, bufs, lens, &n->poseMs)) return 0;
    const float half = 0.5f * patch;
    f->cx = cx + half * coord[0];
    f->cy = cy + half * coord[1];
    f->size = half * coord[2];
    f->qw = quat[3]; f->qx = quat[0]; f->qy = quat[1]; f->qz = quat[2];
    f->box = (ht_rect){cx + half * outBox[0], cy + half * outBox[1], half * (outBox[2] - outBox[0]), half * (outBox[3] - outBox[1])};
    return 1;
}

/* ---------------------------------------------------------------- from a face to yaw and pitch */

static ht_quat ht_mul(ht_quat a, ht_quat b) {
    return (ht_quat){a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                     a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

/* image_to_world(cv::Quatf): the network's axes to the tracker's. It's its own inverse. */
static ht_quat ht_flip_axes(ht_quat q) { return (ht_quat){q.w, -q.z, -q.y, -q.x}; }

/* yaw, pitch, roll in degrees, for one face at (cx, cy) in a picture of this size. */
static void ht_face_to_angles(const ht_face *f, int version, int imgW, int imgH, double fullAspect, double *yaw, double *pitch, double *roll) {
    const double diag = HT_FOV_DEG * 3.14159265358979323846 / 180.0, a2 = fullAspect * fullAspect;
    const double fovW = 2. * atan(tan(diag / 2.) / sqrt(1. + 1. / a2)), fovH = 2. * atan(tan(diag / 2.) / sqrt(1. + a2));
    const float focalW = (float)(1. / tan(.5 * fovW)), focalH = (float)(1. / tan(.5 * fovH));

    /* where the face sits in the world: only its direction matters for the turn */
    const float xpos = -(focalW * imgW * 0.5f) / (2.f * f->size) * 200.f;
    const float zpos = (f->cx / imgW * 2.f - 1.f) * xpos / focalW;
    const float ypos = (f->cy / imgH * 2.f - 1.f) * xpos / focalH;

    /* a head off to the side of the picture looks turned when it isn't: correct for that */
    const float ax = 0.f * zpos - 0.f * ypos, ay = 0.f * xpos - (-1.f) * zpos, az = (-1.f) * ypos - 0.f * xpos;
    const float dot = -xpos, len = sqrtf(ax * ax + ay * ay + az * az);
    float nx = ax / len, ny = ay / len, nz = az / len, angle = atan2f(len, dot);
    if (!(isfinite(nx) && isfinite(ny) && isfinite(nz))) { angle = 0; nx = 1; ny = 0; nz = 0; }
    const float s = sinf(angle * 0.5f), c = cosf(angle * 0.5f);
    const ht_quat correction = {c, s * nx, s * ny, s * nz};

    ht_quat face = {f->qw, f->qx, f->qy, f->qz};
    if (version < 2) face = ht_flip_axes(face);
    const ht_quat q = ht_mul(correction, ht_flip_axes(face));

    const double r00 = 1 - 2 * (q.y * q.y + q.z * q.z), r10 = 2 * (q.x * q.y + q.w * q.z), r20 = 2 * (q.x * q.z - q.w * q.y);
    const double r11 = 1 - 2 * (q.x * q.x + q.z * q.z), r12 = 2 * (q.y * q.z - q.w * q.x);
    const double r2d = 180.0 / 3.14159265358979323846;
    *yaw = r2d * atan2(r20, r00);
    *pitch = r2d * -atan2(-r10, sqrt(r20 * r20 + r00 * r00));
    *roll = -r2d * atan2(-r12, r11);
}

static double ht_iou(const ht_rect *a, const ht_rect *b) {
    const float x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    const float x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    const float i = x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0.f;
    return (double)i / (a->w * a->h + b->w * b->h - i);
}

/* One frame through the pipeline. Keeps following the head from frame to frame, and only
   runs the slower whole-picture search when it's lost or has moved a long way. */
typedef struct {
    ht_nets *nets;
    ht_image level[HT_LEVELS];
    int depth;
    int hasRoi, hasLocRoi;
    ht_rect roi, locRoi;
} ht_vision;

static int ht_detect(ht_vision *v, double *yaw, double *pitch, double *roll) {
    const ht_image *im = &v->level[v->depth - 1];
    if (!v->hasLocRoi || !v->hasRoi || ht_iou(&v->locRoi, &v->roi) < 0.25) {
        ht_rect rect = {0, 0, 0, 0};
        const float p = ht_localize(v->nets, im, &rect);
        if (v->hasRoi && ht_iou(&rect, &v->roi) >= 0.25 && p > 0.5f) {
            v->locRoi = rect; v->hasLocRoi = 1;
        } else if (p > 0.5f && rect.h > 32 && rect.w > 32) {
            v->locRoi = rect; v->hasLocRoi = 1;
            v->roi = rect; v->hasRoi = 1;
        } else {
            v->hasRoi = v->hasLocRoi = 0;
        }
    }
    if (!v->hasRoi) return 0;

    ht_face f;
    if (!ht_estimate_pose(v->nets, im, &v->roi, &f)) {
        v->hasRoi = 0;
        return 0;
    }
    v->roi = f.box;
    ht_face_to_angles(&f, v->nets->poseVersion, im->w, im->h, (double)im->w / im->h, yaw, pitch, roll);
    return 1;
}

/* The picture in grey, cropped to 4:3 and a multiple of 4, then halved until it's at most 640 wide. */
static int ht_set_frame(ht_vision *v, const unsigned char *bgra, LONG pitch, int w, int h) {
    int cw = w, ch = h, ox = 0, oy = 0;
    if (h * 4 != w * 3) {
        if (w * 3 > 4 * h) { cw = (4 * h) / 3; ox = (w - cw) / 2; }
        else { ch = (3 * w) / 4; oy = (h - ch) / 2; }
    }
    const int nw = (cw / 4) * 4, nh = (ch / 4) * 4;
    ox += (cw - nw) / 2;
    oy += (ch - nh) / 2;
    if (nw <= 0 || nh <= 0 || !ht_image_size(&v->level[0], nw, nh)) return 0;
    for (int y = 0; y < nh; y++) {
        const unsigned char *s = bgra + (ptrdiff_t)(y + oy) * pitch + (ptrdiff_t)ox * 4;
        unsigned char *o = v->level[0].p + (size_t)y * nw;
        for (int x = 0; x < nw; x++, s += 4) o[x] = (unsigned char)((s[2] * 4899 + s[1] * 9617 + s[0] * 1868 + 8192) >> 14);
    }
    v->depth = 1;
    while (v->depth < HT_LEVELS && v->level[v->depth - 1].w > 640) {
        ht_pyr_down(&v->level[v->depth - 1], &v->level[v->depth]);
        v->depth++;
    }
    return 1;
}

/* ---------------------------------------------------------------- the camera */

/* These three aren't in reach of the headers without extra libraries. */
static const GUID HT_IID_PropertyBag = {0x55272A00, 0x42CB, 0x11CE, {0x81, 0x35, 0x00, 0xAA, 0x00, 0x4B, 0xB8, 0x51}};
static const GUID HT_SUBTYPE_MJPG = {0x47504A4D, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID HT_CLSID_NullRenderer = {0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

/* Walks the cameras Windows offers: fn gets each one's name and moniker. Return 1 from fn to stop. */
static void ht_each_camera(int (*fn)(const char *name, IMoniker *m, void *ctx), void *ctx) {
    ICreateDevEnum *de = NULL;
    IEnumMoniker *en = NULL;
    IMoniker *m = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, &IID_ICreateDevEnum, (void **)&de))) return;
    if (ICreateDevEnum_CreateClassEnumerator(de, &CLSID_VideoInputDeviceCategory, &en, 0) == S_OK) {
        int stop = 0;
        while (!stop && en->lpVtbl->Next(en, 1, &m, NULL) == S_OK) {
            IPropertyBag *bag = NULL;
            if (SUCCEEDED(m->lpVtbl->BindToStorage(m, NULL, NULL, &HT_IID_PropertyBag, (void **)&bag))) {
                VARIANT var;
                VariantInit(&var);
                if (SUCCEEDED(bag->lpVtbl->Read(bag, L"FriendlyName", &var, NULL)) && var.vt == VT_BSTR) {
                    char name[128] = "";
                    WideCharToMultiByte(CP_ACP, 0, var.bstrVal, -1, name, sizeof(name), NULL, NULL);
                    name[127] = '\0';
                    stop = fn(name, m, ctx);
                }
                VariantClear(&var);
                bag->lpVtbl->Release(bag);
            }
            m->lpVtbl->Release(m);
        }
        en->lpVtbl->Release(en);
    }
    ICreateDevEnum_Release(de);
}

typedef struct { char (*names)[128]; int max, n; } ht_list_ctx;

static int ht_list_one(const char *name, IMoniker *m, void *ctx) {
    (void)m;
    ht_list_ctx *c = (ht_list_ctx *)ctx;
    if (c->n < c->max) {
        strncpy(c->names[c->n], name, 127);
        c->names[c->n][127] = '\0';
        c->n++;
    }
    return 0;
}

/* Every camera Windows offers, virtual ones included. Returns how many fit. */
static int ht_list_cameras(char names[][128], int max) {
    ht_list_ctx c = {names, max, 0};
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ht_each_camera(ht_list_one, &c);
    if (SUCCEEDED(co)) CoUninitialize();
    return c.n;
}

typedef struct {
    IGraphBuilder *graph;
    ICaptureGraphBuilder2 *builder;
    IBaseFilter *source, *grabberFilter, *nullFilter;
    ISampleGrabber *grabber;
    IMediaControl *control;
    int w, h;
    LONG pitch;
} ht_camera;

/* The newest frame, copied in on DirectShow's thread. A frame that arrives while the last one is
   still being read just replaces it, so the tracker always gets the latest and never falls behind. */
static CRITICAL_SECTION g_ht_frame_lock;
static HANDLE g_ht_frame_event = NULL;
static unsigned char *g_ht_frame = NULL;
static size_t g_ht_frame_size = 0, g_ht_frame_len = 0;

static HRESULT STDMETHODCALLTYPE ht_cb_query(ISampleGrabberCB *t, REFIID r, void **o) {
    if (IsEqualGUID(r, &IID_ISampleGrabberCB) || IsEqualGUID(r, &IID_IUnknown)) { *o = t; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ht_cb_addref(ISampleGrabberCB *t) { (void)t; return 2; }
static ULONG STDMETHODCALLTYPE ht_cb_release(ISampleGrabberCB *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE ht_cb_sample(ISampleGrabberCB *t, double when, IMediaSample *sample) {
    (void)t; (void)when;
    BYTE *data = NULL;
    const long len = IMediaSample_GetActualDataLength(sample);
    if (len > 0 && SUCCEEDED(IMediaSample_GetPointer(sample, &data))) {
        EnterCriticalSection(&g_ht_frame_lock);
        if (g_ht_frame_size < (size_t)len) {
            free(g_ht_frame);
            g_ht_frame = (unsigned char *)malloc((size_t)len);
            g_ht_frame_size = g_ht_frame ? (size_t)len : 0;
        }
        if (g_ht_frame) {
            memcpy(g_ht_frame, data, (size_t)len);
            g_ht_frame_len = (size_t)len;
        }
        LeaveCriticalSection(&g_ht_frame_lock);
        SetEvent(g_ht_frame_event);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ht_cb_buffer(ISampleGrabberCB *t, double when, BYTE *b, LONG n) {
    (void)t; (void)when; (void)b; (void)n;
    return E_NOTIMPL;
}
static ISampleGrabberCBVtbl g_ht_cb_vtbl = {ht_cb_query, ht_cb_addref, ht_cb_release, ht_cb_sample, ht_cb_buffer};
static ISampleGrabberCB g_ht_cb = {&g_ht_cb_vtbl};

static void ht_free_media_type(AM_MEDIA_TYPE *mt) {
    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) mt->pUnk->lpVtbl->Release(mt->pUnk);
    CoTaskMemFree(mt);
}

static void ht_camera_close(ht_camera *c) {
    if (c->control) {
        IMediaControl_Stop(c->control);
        IMediaControl_Release(c->control);
    }
    if (c->grabber) {
        ISampleGrabber_SetCallback(c->grabber, NULL, 0);
        ISampleGrabber_Release(c->grabber);
    }
    if (c->nullFilter) IBaseFilter_Release(c->nullFilter);
    if (c->grabberFilter) IBaseFilter_Release(c->grabberFilter);
    if (c->source) IBaseFilter_Release(c->source); /* this is what turns the camera's light off */
    if (c->builder) ICaptureGraphBuilder2_Release(c->builder);
    if (c->graph) IGraphBuilder_Release(c->graph);
    memset(c, 0, sizeof(*c));
}

typedef struct { const char *want; IBaseFilter *filter; } ht_find_ctx;

static int ht_find_one(const char *name, IMoniker *m, void *ctx) {
    ht_find_ctx *c = (ht_find_ctx *)ctx;
    if (strcmp(name, c->want)) return 0;
    return SUCCEEDED(m->lpVtbl->BindToObject(m, NULL, NULL, &IID_IBaseFilter, (void **)&c->filter));
}

/* Asks for the closest mode to the one wanted, preferring MJPEG. A camera with no such
   control, which some virtual ones are, just keeps its own mode. */
static void ht_camera_pick_mode(ht_camera *c, int wantW, int wantH, int wantFps) {
    IAMStreamConfig *cfg = NULL;
    int count = 0, size = 0;
    if (FAILED(ICaptureGraphBuilder2_FindInterface(c->builder, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, c->source, &IID_IAMStreamConfig, (void **)&cfg))) return;
    AM_MEDIA_TYPE *best = NULL;
    double bestScore = 1e30, bestFps = 0;
    if (SUCCEEDED(IAMStreamConfig_GetNumberOfCapabilities(cfg, &count, &size)) && size == sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
        for (int i = 0; i < count; i++) {
            AM_MEDIA_TYPE *mt = NULL;
            VIDEO_STREAM_CONFIG_CAPS caps;
            if (FAILED(IAMStreamConfig_GetStreamCaps(cfg, i, &mt, (BYTE *)&caps))) continue;
            if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo) || mt->cbFormat < sizeof(VIDEOINFOHEADER)) {
                ht_free_media_type(mt);
                continue;
            }
            const VIDEOINFOHEADER *vih = (const VIDEOINFOHEADER *)mt->pbFormat;
            const double top = caps.MinFrameInterval > 0 ? 1e7 / caps.MinFrameInterval : vih->AvgTimePerFrame > 0 ? 1e7 / vih->AvgTimePerFrame : 0;
            const double fps = top < wantFps ? top : wantFps;
            double score = fabs((double)vih->bmiHeader.biWidth * labs(vih->bmiHeader.biHeight) - (double)wantW * wantH) / 1000.0 + fabs(fps - wantFps) * 100.0;
            if (!IsEqualGUID(&mt->subtype, &HT_SUBTYPE_MJPG)) score += 1.0;
            if (score < bestScore) {
                if (best) ht_free_media_type(best);
                best = mt;
                bestScore = score;
                bestFps = fps;
            } else {
                ht_free_media_type(mt);
            }
        }
    }
    if (best) {
        if (bestFps > 0) ((VIDEOINFOHEADER *)best->pbFormat)->AvgTimePerFrame = (REFERENCE_TIME)(1e7 / bestFps);
        IAMStreamConfig_SetFormat(cfg, best);
        ht_free_media_type(best);
    }
    IAMStreamConfig_Release(cfg);
}

/* Camera -> whatever decoder Windows needs -> a grabber asking for 32-bit frames -> nothing. */
static int ht_camera_open(ht_camera *c, const char *name, int wantW, int wantH, int wantFps) {
    ht_find_ctx find = {name, NULL};
    ht_each_camera(ht_find_one, &find);
    c->source = find.filter;
    if (!c->source) { log_line("head tracking: couldn't open the camera \"%s\"", name); return 0; }
    AM_MEDIA_TYPE want;
    memset(&want, 0, sizeof(want));
    want.majortype = MEDIATYPE_Video;
    want.subtype = MEDIASUBTYPE_RGB32;
    want.formattype = FORMAT_VideoInfo;
    HRESULT hr;
    if (FAILED(hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (void **)&c->graph)) ||
        FAILED(hr = CoCreateInstance(&CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, &IID_ICaptureGraphBuilder2, (void **)&c->builder)) ||
        FAILED(hr = ICaptureGraphBuilder2_SetFiltergraph(c->builder, c->graph)) ||
        FAILED(hr = CoCreateInstance(&CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter, (void **)&c->grabberFilter)) ||
        FAILED(hr = IBaseFilter_QueryInterface(c->grabberFilter, &IID_ISampleGrabber, (void **)&c->grabber)) ||
        FAILED(hr = CoCreateInstance(&HT_CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter, (void **)&c->nullFilter)) ||
        FAILED(hr = IGraphBuilder_AddFilter(c->graph, c->source, L"camera")) ||
        FAILED(hr = IGraphBuilder_AddFilter(c->graph, c->grabberFilter, L"grabber")) ||
        FAILED(hr = IGraphBuilder_AddFilter(c->graph, c->nullFilter, L"null")) ||
        FAILED(hr = ISampleGrabber_SetMediaType(c->grabber, &want))) {
        log_line("head tracking: couldn't set up the camera (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    ht_camera_pick_mode(c, wantW, wantH, wantFps);
    hr = ICaptureGraphBuilder2_RenderStream(c->builder, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, (IUnknown *)c->source, c->grabberFilter, c->nullFilter);
    if (FAILED(hr)) hr = ICaptureGraphBuilder2_RenderStream(c->builder, NULL, &MEDIATYPE_Video, (IUnknown *)c->source, c->grabberFilter, c->nullFilter);
    if (FAILED(hr)) { log_line("head tracking: couldn't read this camera's video (0x%08lx)", (unsigned long)hr); return 0; }

    AM_MEDIA_TYPE got;
    memset(&got, 0, sizeof(got));
    if (FAILED(hr = ISampleGrabber_GetConnectedMediaType(c->grabber, &got))) { log_line("head tracking: no video format (0x%08lx)", (unsigned long)hr); return 0; }
    if (got.cbFormat >= sizeof(VIDEOINFOHEADER) && got.pbFormat) {
        const BITMAPINFOHEADER *bi = &((const VIDEOINFOHEADER *)got.pbFormat)->bmiHeader;
        c->w = bi->biWidth;
        c->h = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
        c->pitch = bi->biHeight < 0 ? c->w * 4 : -(c->w * 4); /* 32-bit frames normally arrive bottom row first */
    }
    if (got.cbFormat) CoTaskMemFree(got.pbFormat);
    if (got.pUnk) got.pUnk->lpVtbl->Release(got.pUnk);
    if (c->w <= 0 || c->h <= 0) { log_line("head tracking: no video size"); return 0; }

    /* no clock: every frame comes through as soon as it exists */
    IMediaFilter *mf = NULL;
    if (SUCCEEDED(IGraphBuilder_QueryInterface(c->graph, &IID_IMediaFilter, (void **)&mf))) {
        IMediaFilter_SetSyncSource(mf, NULL);
        IMediaFilter_Release(mf);
    }
    ISampleGrabber_SetBufferSamples(c->grabber, FALSE);
    ISampleGrabber_SetOneShot(c->grabber, FALSE);
    ISampleGrabber_SetCallback(c->grabber, &g_ht_cb, 0);
    if (FAILED(hr = IGraphBuilder_QueryInterface(c->graph, &IID_IMediaControl, (void **)&c->control)) || FAILED(hr = IMediaControl_Run(c->control))) {
        log_line("head tracking: the camera wouldn't start (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- the Accela filter */

#define HT_SPLINE_COUNT 16384
static float g_ht_spline[HT_SPLINE_COUNT];
static double g_ht_spline_c = 0;

/* The gain curve is Catmull-Rom through these points, looked up per bucket, as opentrack does. */
static void ht_make_spline(void) {
    static const double pts[8][2] = {{0, 0}, {.5, .4}, {1, 1.5}, {1.5, 8}, {2.5, 35}, {5, 100}, {8, 200}, {9, 300}};
    const int n = 8;
    if (g_ht_spline_c > 0) return;
    const double c = (HT_SPLINE_COUNT - 1) / 9.0;
    for (int i = 0; i + 1 < n; i++) {
        const double *p0 = pts[i ? i - 1 : 0], *p1 = pts[i], *p2 = pts[i + 1], *p3 = pts[i + 2 < n ? i + 2 : n - 1];
        if (i == 0) { static const double zero[2] = {0, 0}; p0 = zero; }
        const double cx[4] = {2 * p1[0], -p0[0] + p2[0], 2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0], -p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]};
        const double cy[4] = {2 * p1[1], -p0[1] + p2[1], 2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1], -p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]};
        int start = (int)ceil(p1[0] * c), end = (int)floor(p2[0] * c);
        if (end > HT_SPLINE_COUNT - 1) end = HT_SPLINE_COUNT - 1;
        for (int k = start; k <= end; k++) {
            const double X = k / c;
            double lo = 0, hi = 1;
            for (int it = 0; it < 48; it++) {
                const double t = (lo + hi) * 0.5;
                const double x = .5 * (cx[0] + cx[1] * t + cx[2] * t * t + cx[3] * t * t * t);
                if (x < X) lo = t; else hi = t;
            }
            const double t = (lo + hi) * 0.5;
            const double y = .5 * (cy[0] + cy[1] * t + cy[2] * t * t + cy[3] * t * t * t);
            g_ht_spline[k] = (float)(y < 0 ? 0 : y > 300 ? 300 : y);
        }
    }
    g_ht_spline_c = c;
}

static double ht_spline(double x) {
    const double q = x * g_ht_spline_c;
    const int i = (int)q;
    const double f = q - i;
    const float a = g_ht_spline[i < HT_SPLINE_COUNT ? i : HT_SPLINE_COUNT - 1];
    const float b = g_ht_spline[i + 1 < HT_SPLINE_COUNT ? i + 1 : HT_SPLINE_COUNT - 1];
    return b * f + a * (1 - f);
}

/* in and out are yaw, pitch, roll. Smoothing 1.5, dead zone 0.03: opentrack's defaults. */
static void ht_accela(const double in[3], double last[3], double dt) {
    double d[3], dist = 0, n = 0, norm[3];
    for (int k = 0; k < 3; k++) {
        double v = in[k] - last[k];
        if (fabs(v) > 180) v -= copysign(360, v);
        v = fabs(v) > .03 ? v - copysign(.03, v) : 0;
        d[k] = v / 1.5;
        dist += d[k] * d[k];
    }
    dist = sqrt(dist);
    const double value = ht_spline(dist);
    for (int k = 0; k < 3; k++) {
        double r = dist > 1e-6 ? fabs(d[k]) / dist : 0;
        norm[k] = r > 1 ? 1 : r;
        n += norm[k];
    }
    for (int k = 0; k < 3; k++) {
        const double share = n > 1e-6 ? norm[k] / n : 0;
        const double out = (d[k] > 0 ? 1 : d[k] < 0 ? -1 : 0) * share * value * dt + last[k];
        last[k] = fabs(out) > 180 ? out - copysign(360, out) : out;
    }
}

/* ---------------------------------------------------------------- centering */

static ht_quat ht_from_euler(double pitch, double yaw, double roll) {
    const double c = 3.14159265358979323846 / 360.0;
    const double c1 = cos(yaw * c), s1 = sin(yaw * c), c2 = cos(roll * c), s2 = sin(roll * c), c3 = cos(pitch * c), s3 = sin(pitch * c);
    return (ht_quat){c1 * c2 * c3 + s1 * s2 * s3, c1 * c2 * s3 + s1 * s2 * c3, s1 * c2 * c3 - c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3};
}

/* opentrack's default "roll compensated" centering: angles measured from where the head was
   when tracking started, which matters when the camera isn't straight in front of you. */
static void ht_apply_center(double v[3], const double center[3], ht_quat qr) {
    ht_quat q = ht_mul(qr, ht_from_euler(v[1], v[0], center[2] - v[2]));
    const double len = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z), inv = len >= 1e-6 ? 1 / len : 1;
    const double x = q.x * inv, y = q.y * inv, z = q.z * inv, w = q.w * inv;
    double s = -2.0 * (y * z - x * w);
    s = s < -1 ? -1 : s > 1 ? 1 : s;
    const double r2d = 180.0 / 3.14159265358979323846;
    v[1] = r2d * asin(s);
    v[0] = r2d * atan2(2 * (x * z + y * w), 1 - 2 * (x * x + y * y));
    v[2] = -r2d * atan2(2 * (x * y + z * w), 1 - 2 * (x * x + z * z));
}

/* ---------------------------------------------------------------- the two threads */

static CRITICAL_SECTION g_ht_lock;
static volatile LONG g_ht_lock_ready = 0;
static volatile LONG g_ht_stop = 0;
static HANDLE g_ht_threads[2] = {NULL, NULL};
static double g_ht_raw[3];              /* latest yaw, pitch, roll from the network */
static int g_ht_raw_seq = 0;            /* counts frames with a head in them */
static int g_ht_raw_valid = 0;          /* the head was found in the latest frame */
static double g_ht_fps = 0, g_ht_net_ms = 0; /* for the test tool */
static char g_ht_camera_name[128];
static int g_ht_want_w, g_ht_want_h, g_ht_want_fps;
static void (*g_ht_on_pose)(double yaw, double pitch);
static void (*g_ht_on_frame)(const ht_image *full, double yaw, double pitch, double roll, int valid); /* for the test tool */

/* Camera to angles, as fast as frames arrive. */
static DWORD WINAPI ht_vision_thread(LPVOID arg) {
    (void)arg;
    ht_camera cam = {0};
    ht_vision *v = (ht_vision *)calloc(1, sizeof(ht_vision));
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (v && (v->nets = ht_nets_open()) && ht_camera_open(&cam, g_ht_camera_name, g_ht_want_w, g_ht_want_h, g_ht_want_fps)) {
        log_line("head tracking: camera on, %dx%d", cam.w, cam.h);
        LARGE_INTEGER fq, last, now;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&last);
        DWORD lastFrame = GetTickCount();
        while (!g_ht_stop) {
            if (WaitForSingleObject(g_ht_frame_event, 200) != WAIT_OBJECT_0) {
                if (GetTickCount() - lastFrame > 5000) { /* nothing for 5 s: unplugged, or taken by something else */
                    log_line("head tracking: the camera stopped sending video");
                    break;
                }
                continue;
            }
            lastFrame = GetTickCount();
            int ok = 0;
            double a[3] = {0, 0, 0};
            EnterCriticalSection(&g_ht_frame_lock);
            const size_t need = (size_t)cam.w * (size_t)cam.h * 4;
            if (g_ht_frame && g_ht_frame_len >= need) {
                const unsigned char *scan = cam.pitch < 0 ? g_ht_frame + (size_t)(cam.h - 1) * (size_t)(-cam.pitch) : g_ht_frame;
                ok = ht_set_frame(v, scan, cam.pitch, cam.w, cam.h);
            }
            LeaveCriticalSection(&g_ht_frame_lock);
            if (!ok) continue;

            const int found = ht_detect(v, &a[0], &a[1], &a[2]);
            EnterCriticalSection(&g_ht_lock);
            g_ht_raw_valid = found;
            if (found) {
                memcpy(g_ht_raw, a, sizeof(a));
                g_ht_raw_seq++;
            }
            QueryPerformanceCounter(&now);
            const double dt = (double)(now.QuadPart - last.QuadPart) / (double)fq.QuadPart;
            last = now;
            if (dt > 1e-6) g_ht_fps += (1.0 / dt - g_ht_fps) * 0.05;
            g_ht_net_ms = v->nets->poseMs;
            LeaveCriticalSection(&g_ht_lock);
            if (g_ht_on_frame) g_ht_on_frame(&v->level[0], a[0], a[1], a[2], found);
        }
    }
    ht_camera_close(&cam);
    if (v) {
        if (v->nets) ht_nets_close(v->nets);
        for (int i = 0; i < HT_LEVELS; i++) free(v->level[i].p);
        free(v);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

/* Smooths toward the latest angles 250 times a second, far more often than frames arrive.
   This is a big part of why it feels smooth. */
static DWORD WINAPI ht_filter_thread(LPVOID arg) {
    (void)arg;
    double center[3] = {0, 0, 0}, out[3] = {0, 0, 0};
    ht_quat qr = {1, 0, 0, 0};
    int started = 0;
    LARGE_INTEGER fq, last, now;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&last);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!g_ht_stop) {
        LARGE_INTEGER due;
        due.QuadPart = -10000LL * HT_FILTER_MS;
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 50);
        else Sleep(1);

        double v[3];
        int valid;
        EnterCriticalSection(&g_ht_lock);
        memcpy(v, g_ht_raw, sizeof(v));
        valid = g_ht_raw_valid;
        LeaveCriticalSection(&g_ht_lock);

        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / (double)fq.QuadPart;
        if (!started) {
            last = now;
            if (!valid) continue;
            started = 1;
            memcpy(center, v, sizeof(center));
            ht_quat q = ht_from_euler(v[1], v[0], 0);
            qr = (ht_quat){q.w, -q.x, -q.y, -q.z};
            ht_apply_center(v, center, qr);
            memcpy(out, v, sizeof(out));
        } else {
            last = now;
            if (dt > 0.05) dt = 0.05; /* a stalled thread eases back rather than jumps */
            ht_apply_center(v, center, qr);
            ht_accela(v, out, dt);
        }
        /* a lost head holds its last reading, and poses keep coming */
        if (g_ht_on_pose) g_ht_on_pose(out[0], -out[1]);
    }
    if (timer) CloseHandle(timer);
    return 0;
}

static int ht_running(void) { return g_ht_threads[0] != NULL; }
static int ht_alive(void) { return g_ht_threads[0] && WaitForSingleObject(g_ht_threads[0], 0) == WAIT_TIMEOUT; }

/* Turns the camera on and starts following the head. */
static int ht_start(const char *camera, int w, int h, int fps, void (*onPose)(double yaw, double pitch)) {
    if (ht_running()) return 1;
    if (!InterlockedExchange(&g_ht_lock_ready, 1)) {
        InitializeCriticalSection(&g_ht_lock);
        InitializeCriticalSection(&g_ht_frame_lock);
        g_ht_frame_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    ht_make_spline();
    strncpy(g_ht_camera_name, camera, sizeof(g_ht_camera_name) - 1);
    g_ht_camera_name[sizeof(g_ht_camera_name) - 1] = '\0';
    g_ht_want_w = w;
    g_ht_want_h = h;
    g_ht_want_fps = fps;
    g_ht_on_pose = onPose;
    g_ht_raw_valid = 0;
    g_ht_stop = 0;
    g_ht_threads[0] = CreateThread(NULL, 0, ht_vision_thread, NULL, 0, NULL);
    g_ht_threads[1] = CreateThread(NULL, 0, ht_filter_thread, NULL, 0, NULL);
    return g_ht_threads[0] && g_ht_threads[1];
}

/* Turns the camera off and frees everything. Waits, so the camera really has been let go. */
static void ht_stop(void) {
    if (!ht_running()) return;
    InterlockedExchange(&g_ht_stop, 1);
    for (int i = 0; i < 2; i++) {
        if (g_ht_threads[i]) {
            if (WaitForSingleObject(g_ht_threads[i], 3000) != WAIT_OBJECT_0) log_line("head tracking: a thread was slow to stop");
            CloseHandle(g_ht_threads[i]);
            g_ht_threads[i] = NULL;
        }
    }
}
