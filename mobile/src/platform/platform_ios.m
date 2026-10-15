/* iOS: VisionKit document camera for pages, camera + Vision for labels (QR codes and handwritten
 * numbers), Vision text recognition for OCR, share sheet for printing labels, NSURLSession for HTTP.
 * All OCR runs on-device, so scanning and search work with no network. */
#import <UIKit/UIKit.h>
#import <Vision/Vision.h>
#import <VisionKit/VisionKit.h>

#include "platform.h"

static SDL_Window *g_window;
static char g_tmp_dir[1024];
static Uint32 g_scan_event, g_label_event;

static UIViewController *root_controller(void)
{
    UIWindow *w = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    UIViewController *vc = w.rootViewController;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    return vc;
}

/* ---- OCR -------------------------------------------------------------------------------------- */

static NSArray<VNRecognizedTextObservation *> *recognize_text(CGImageRef image)
{
    VNRecognizeTextRequest *req = [[VNRecognizeTextRequest alloc] init];
    req.recognitionLevel = VNRequestTextRecognitionLevelAccurate; /* the accurate model also reads handwriting */
    req.usesLanguageCorrection = YES;
    if (@available(iOS 16.0, *)) req.automaticallyDetectsLanguage = YES;
    VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
    NSError *err = nil;
    if (![h performRequests:@[req] error:&err]) return @[];
    return req.results ?: @[];
}

static char *ocr_lines(CGImageRef image)
{
    NSMutableArray<NSString *> *lines = [NSMutableArray array];
    for (VNRecognizedTextObservation *o in recognize_text(image)) {
        VNRecognizedText *best = [o topCandidates:1].firstObject;
        if (best.string.length) [lines addObject:best.string];
    }
    return SDL_strdup([[lines componentsJoinedByString:@"\n"] UTF8String] ?: "");
}

static NSString *find_qr(CGImageRef image)
{
    VNDetectBarcodesRequest *req = [[VNDetectBarcodesRequest alloc] init];
    req.symbologies = @[VNBarcodeSymbologyQR];
    VNImageRequestHandler *h = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
    if (![h performRequests:@[req] error:nil]) return nil;
    for (VNBarcodeObservation *o in req.results) {
        if (o.payloadStringValue.length) return o.payloadStringValue;
    }
    return nil;
}

/* Scale to at most 2400px on the long side and write a JPEG into the tmp dir. */
static UIImage *normalized(UIImage *img)
{
    CGFloat longest = MAX(img.size.width * img.scale, img.size.height * img.scale);
    CGFloat k = longest > 2400 ? 2400 / longest : 1;
    CGSize size = CGSizeMake(round(img.size.width * img.scale * k), round(img.size.height * img.scale * k));
    UIGraphicsImageRendererFormat *fmt = [UIGraphicsImageRendererFormat defaultFormat];
    fmt.scale = 1;
    UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:size format:fmt];
    return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) { [img drawInRect:CGRectMake(0, 0, size.width, size.height)]; }];
}

static bool save_page(UIImage *img, ScanPage *page)
{
    UIImage *n = normalized(img);
    NSData *jpeg = UIImageJPEGRepresentation(n, 0.82);
    SDL_snprintf(page->path, sizeof page->path, "%sscan-%llu-%u.jpg", g_tmp_dir, (unsigned long long)SDL_GetTicksNS(), SDL_rand_bits());
    if (![jpeg writeToFile:[NSString stringWithUTF8String:page->path] atomically:YES]) return false;
    page->width = (int)n.size.width;
    page->height = (int)n.size.height;
    page->ocr_text = ocr_lines(n.CGImage);
    return true;
}

/* ---- presenters ------------------------------------------------------------------------------- */

@interface MFScanDelegate : NSObject <VNDocumentCameraViewControllerDelegate, UIImagePickerControllerDelegate, UINavigationControllerDelegate>
@property(nonatomic) BOOL label; /* picker used for a label scan */
@end

static MFScanDelegate *g_delegate; /* UIKit holds delegates weakly */

static void post_cancel(BOOL label)
{
    if (label) {
        LabelResult *r = label_result_new();
        r->cancelled = true;
        platform_post(g_label_event, r);
    } else {
        ScanResult *r = scan_result_new(0);
        r->cancelled = true;
        platform_post(g_scan_event, r);
    }
}

@implementation MFScanDelegate

- (void)documentCameraViewController:(VNDocumentCameraViewController *)controller didFinishWithScan:(VNDocumentCameraScan *)scan
{
    NSMutableArray<UIImage *> *images = [NSMutableArray array];
    for (NSUInteger i = 0; i < scan.pageCount; i++) [images addObject:[scan imageOfPageAtIndex:i]];
    [controller dismissViewControllerAnimated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ScanResult *r = scan_result_new((int)images.count);
        int n = 0;
        for (UIImage *img in images) {
            @autoreleasepool {
                if (save_page(img, &r->pages[n])) n++;
            }
        }
        r->count = n;
        platform_post(g_scan_event, r);
    });
}

- (void)documentCameraViewControllerDidCancel:(VNDocumentCameraViewController *)controller
{
    [controller dismissViewControllerAnimated:YES completion:nil];
    post_cancel(NO);
}

- (void)documentCameraViewController:(VNDocumentCameraViewController *)controller didFailWithError:(NSError *)error
{
    [controller dismissViewControllerAnimated:YES completion:nil];
    ScanResult *r = scan_result_new(0);
    SDL_strlcpy(r->error, error.localizedDescription.UTF8String ?: "Scan failed", sizeof r->error);
    platform_post(g_scan_event, r);
}

- (void)imagePickerController:(UIImagePickerController *)picker didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey, id> *)info
{
    UIImage *img = info[UIImagePickerControllerOriginalImage];
    BOOL label = self.label;
    [picker dismissViewControllerAnimated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (label) {
            LabelResult *r = label_result_new();
            UIImage *n = normalized(img);
            NSString *qr = find_qr(n.CGImage);
            if (qr) {
                r->text = SDL_strdup(qr.UTF8String);
                r->from_qr = true;
            } else {
                r->text = ocr_lines(n.CGImage);
            }
            platform_post(g_label_event, r);
        } else {
            ScanResult *r = scan_result_new(1);
            r->count = save_page(img, &r->pages[0]) ? 1 : 0;
            platform_post(g_scan_event, r);
        }
    });
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)picker
{
    BOOL label = self.label;
    [picker dismissViewControllerAnimated:YES completion:nil];
    post_cancel(label);
}

@end

void platform_init(SDL_Window *window, const char *tmp_dir, Uint32 scan_event, Uint32 label_event)
{
    g_window = window;
    SDL_strlcpy(g_tmp_dir, tmp_dir, sizeof g_tmp_dir);
    g_scan_event = scan_event;
    g_label_event = label_event;
    g_delegate = [[MFScanDelegate alloc] init];
}

const char *platform_font_dir(void)
{
    static char dir[1024];
    if (!dir[0]) SDL_snprintf(dir, sizeof dir, "%sfonts/", SDL_GetBasePath());
    return dir;
}

static void present_picker(BOOL label)
{
    UIImagePickerController *p = [[UIImagePickerController alloc] init];
    /* The simulator has no camera: fall back to the photo library. */
    p.sourceType = [UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypeCamera]
                       ? UIImagePickerControllerSourceTypeCamera : UIImagePickerControllerSourceTypePhotoLibrary;
    g_delegate.label = label;
    p.delegate = g_delegate;
    [root_controller() presentViewController:p animated:YES completion:nil];
}

void platform_scan_document(void)
{
    if (VNDocumentCameraViewController.isSupported) {
        VNDocumentCameraViewController *vc = [[VNDocumentCameraViewController alloc] init];
        vc.delegate = g_delegate;
        [root_controller() presentViewController:vc animated:YES completion:nil];
    } else {
        present_picker(NO);
    }
}

void platform_scan_label(void) { present_picker(YES); }

void platform_share_file(const char *path, const char *mime)
{
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    UIActivityViewController *vc = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
    UIViewController *root = root_controller();
    vc.popoverPresentationController.sourceView = root.view; /* iPad */
    vc.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(root.view.bounds), CGRectGetMidY(root.view.bounds), 1, 1);
    [root presentViewController:vc animated:YES completion:nil];
}

/* ---- HTTP ------------------------------------------------------------------------------------- */

bool platform_http(const HttpRequest *req, HttpResponse *res)
{
    SDL_zerop(res);
    @autoreleasepool {
        NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:[NSString stringWithUTF8String:req->url]]];
        r.HTTPMethod = [NSString stringWithUTF8String:req->method];
        r.timeoutInterval = 120;
        if (req->bearer) [r setValue:[NSString stringWithFormat:@"Bearer %s", req->bearer] forHTTPHeaderField:@"Authorization"];
        if (req->content_type) [r setValue:[NSString stringWithUTF8String:req->content_type] forHTTPHeaderField:@"Content-Type"];
        if (req->body) r.HTTPBody = [NSData dataWithBytes:req->body length:req->body_len];

        __block NSData *data = nil;
        __block NSInteger status = 0;
        __block BOOL ok = NO;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[NSURLSession.sharedSession dataTaskWithRequest:r completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
            if (!err) {
                ok = YES;
                data = d;
                status = ((NSHTTPURLResponse *)resp).statusCode;
            }
            dispatch_semaphore_signal(done);
        }] resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (!ok) return false;
        res->status = (int)status;
        res->len = data.length;
        res->body = SDL_malloc(res->len + 1);
        if (res->len) SDL_memcpy(res->body, data.bytes, res->len);
        res->body[res->len] = '\0';
        return true;
    }
}
