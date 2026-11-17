#import <UIKit/UIKit.h>
#import <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include "hle/input.h"
#include "touch_hunter.h"
#include "overlay.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t buttons;
static int axes[4];
static int menuRequested;
static _Atomic int controllerConnected;
#define BIT(x) (1ull << (x))
typedef struct { CGFloat x,y,r; uint64_t mask; int icon; __unsafe_unretained NSString *name; } Key;

@interface HunterPad : UIView
- (void)setPhysicalControllerConnected:(BOOL)connected;
@end
static __weak HunterPad *activePad;
@implementation HunterPad {
    NSMapTable<UITouch *, NSNumber *> *owners;
    UITouch *leftTouch, *cameraTouch;
    CGPoint leftPoint, cameraStart, cameraPoint;
    Key keys[18]; int count;
    CGFloat scale, width, height, leftEdge, rightEdge, bottomEdge;
    UIImage *atlas;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = UIColor.clearColor; self.multipleTouchEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        owners = [NSMapTable strongToStrongObjectsMapTable];
        NSString *path = [[NSBundle mainBundle] pathForResource:@"hunter-glyphs-atlas" ofType:@"png"];
        atlas = path ? [UIImage imageWithContentsOfFile:path] : nil;
        if (!atlas.CGImage) atlas = [UIImage imageNamed:@"hunter-glyphs-atlas.png"];
        if (!atlas.CGImage) NSLog(@"[hunter-pad] Icon atlas missing or unreadable; drawing text labels");
    }
    return self;
}
- (void)setPhysicalControllerConnected:(BOOL)connected {
    if (connected) {
        [owners removeAllObjects]; leftTouch = nil; cameraTouch = nil;
        pthread_mutex_lock(&mu); buttons = 0; memset(axes, 0, sizeof axes); menuRequested = 0; pthread_mutex_unlock(&mu);
    }
    self.hidden = connected;
    self.userInteractionEnabled = !connected;
    if (!connected) [self setNeedsDisplay];
}
- (void)buildKeys {
    width=self.bounds.size.width; height=self.bounds.size.height;
    UIEdgeInsets safe=self.safeAreaInsets;
    leftEdge=safe.left+4; rightEdge=width-safe.right-4; bottomEdge=height-safe.bottom-4;
    CGFloat availableWidth=MAX(1,rightEdge-leftEdge);
    CGFloat availableHeight=MAX(1,bottomEdge-safe.top);
    scale=MIN(1.15,MIN(availableHeight/430.0,availableWidth/700.0));
    CGFloat s=scale, b=bottomEdge, top=safe.top+4; count=0;
#define ADD(X,Y,R,M,I,N) keys[count++] = (Key){(X),(Y),(R),(M),(I),(N)}
    ADD(rightEdge-48*s,b-72*s,32*s,BIT(BTN_X),0,@"攻击 X");
    ADD(rightEdge-118*s,b-133*s,25*s,BIT(BTN_A),2,@"攻击 A");
    ADD(rightEdge-121*s,b-62*s,25*s,BIT(BTN_B),1,@"闪避 B");
    ADD(rightEdge-193*s,b-62*s,23*s,BIT(BTN_Y),3,@"道具 Y");
    ADD(rightEdge-43*s,b-161*s,24*s,BIT(BTN_R),4,@"动作 R");
    ADD(rightEdge-195*s,b-137*s,22*s,BIT(BTN_X)|BIT(BTN_A),8,@"X + A");
    ADD(rightEdge-195*s,b-206*s,22*s,BIT(BTN_R)|BIT(BTN_X)|BIT(BTN_A),12,@"R + X + A");
    ADD(leftEdge+28*s,top+28*s,22*s,BIT(BTN_L),6,@"视角 L");
    ADD(leftEdge+28*s,top+94*s,22*s,BIT(BTN_ZL),6,@"锁定 ZL");
    ADD(rightEdge-28*s,top+28*s,22*s,BIT(BTN_ZR),5,@"ZR");
    ADD(leftEdge+193*s,b-145*s,20*s,BIT(BTN_UP),9,@"狩技 ↑");
    ADD(leftEdge+149*s,b-101*s,20*s,BIT(BTN_LEFT),9,@"狩技 ←");
    ADD(leftEdge+237*s,b-101*s,20*s,BIT(BTN_RIGHT),9,@"狩技 →");
    ADD(leftEdge+193*s,b-57*s,20*s,BIT(BTN_DOWN),9,@"狩技 ↓");
    CGFloat mid=(leftEdge+rightEdge)/2;
    ADD(mid-54*s,top+25*s,18*s,BIT(BTN_MINUS),12,@"特殊 −");
    ADD(mid+12*s,top+25*s,18*s,BIT(BTN_PLUS),14,@"菜单 +");
    ADD(mid+78*s,top+25*s,18*s,0,15,@"模组");
#undef ADD
}
- (void)layoutSubviews { [super layoutSubviews]; [self buildKeys]; [self setNeedsDisplay]; }
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (void)drawRect:(CGRect)rect {
    (void)rect; CGContextRef ctx=UIGraphicsGetCurrentContext();
    UIFont *font=[UIFont systemFontOfSize:MAX(8,9*scale) weight:UIFontWeightMedium];
    NSDictionary *attrs=@{NSFontAttributeName:font, NSForegroundColorAttributeName:[UIColor colorWithRed:0.96 green:0.91 blue:0.79 alpha:0.95]};
    for (int i=0;i<count;i++) {
        Key k=keys[i]; CGRect r=CGRectMake(k.x-k.r,k.y-k.r,2*k.r,2*k.r);
        [[UIColor colorWithWhite:0.04 alpha:0.30] setFill]; [[UIBezierPath bezierPathWithOvalInRect:r] fill];
        [[UIColor colorWithRed:0.75 green:0.61 blue:0.41 alpha:0.65] setStroke]; UIBezierPath *p=[UIBezierPath bezierPathWithOvalInRect:r]; p.lineWidth=1.4*scale; [p stroke];
        if (atlas.CGImage) { size_t w=CGImageGetWidth(atlas.CGImage),h=CGImageGetHeight(atlas.CGImage); int col=k.icon%4,row=k.icon/4;
            CGImageRef part=CGImageCreateWithImageInRect(atlas.CGImage,CGRectMake(col*w/4,row*h/4,w/4,h/4));
            if(part){ UIImage *glyph=[UIImage imageWithCGImage:part]; CGFloat z=k.r*1.15; [glyph drawInRect:CGRectMake(k.x-z/2,k.y-z/2,z,z) blendMode:kCGBlendModeNormal alpha:0.86]; CGImageRelease(part); }
        } else {
            NSString *fallback = [[k.name componentsSeparatedByString:@" "] lastObject];
            UIFont *keyFont = [UIFont boldSystemFontOfSize:MAX(10, k.r * 0.7)];
            NSDictionary *keyAttrs = @{NSFontAttributeName:keyFont, NSForegroundColorAttributeName:UIColor.whiteColor};
            CGSize z = [fallback sizeWithAttributes:keyAttrs];
            [fallback drawAtPoint:CGPointMake(k.x-z.width/2,k.y-z.height/2) withAttributes:keyAttrs];
        }
        CGSize t=[k.name sizeWithAttributes:attrs]; [k.name drawAtPoint:CGPointMake(k.x-t.width/2,k.y+k.r+2*scale) withAttributes:attrs];
    }
    CGPoint c=CGPointMake(leftEdge+66*scale,bottomEdge-77*scale); CGFloat r=57*scale;
    CGContextSetFillColorWithColor(ctx,[UIColor colorWithWhite:0.04 alpha:0.20].CGColor); CGContextFillEllipseInRect(ctx,CGRectMake(c.x-r,c.y-r,2*r,2*r));
    CGContextSetStrokeColorWithColor(ctx,[UIColor colorWithRed:0.75 green:0.61 blue:0.41 alpha:0.55].CGColor); CGContextSetLineWidth(ctx,1.5*scale); CGContextStrokeEllipseInRect(ctx,CGRectMake(c.x-r,c.y-r,2*r,2*r));
    CGPoint knob=c;
    if(leftTouch) { CGFloat dx=leftPoint.x-c.x,dy=leftPoint.y-c.y,d=hypot(dx,dy),limit=40*scale; if(d>limit){dx*=limit/d;dy*=limit/d;} knob=CGPointMake(c.x+dx,c.y+dy); }
    CGFloat kr=22*scale; CGContextSetFillColorWithColor(ctx,[UIColor colorWithRed:0.78 green:0.65 blue:0.43 alpha:0.40].CGColor); CGContextFillEllipseInRect(ctx,CGRectMake(knob.x-kr,knob.y-kr,2*kr,2*kr));
    CGContextSetStrokeColorWithColor(ctx,[UIColor colorWithRed:0.90 green:0.78 blue:0.58 alpha:0.78].CGColor); CGContextStrokeEllipseInRect(ctx,CGRectMake(knob.x-kr,knob.y-kr,2*kr,2*kr));
}
- (void)publish {
    uint64_t m=0; int a[4]={0};
    for (UITouch *t in owners) { int idn=[[owners objectForKey:t] intValue]; if(idn>=0 && idn<count) m|=keys[idn].mask; }
    if(leftTouch) { CGPoint c=CGPointMake(leftEdge+66*scale,bottomEdge-77*scale); CGFloat dx=(leftPoint.x-c.x)/(48*scale),dy=(c.y-leftPoint.y)/(48*scale),len=hypot(dx,dy); if(len>1)dx/=len,dy/=len; if(len>0.1)a[0]=dx*32767,a[1]=dy*32767; }
    if(cameraTouch) { CGFloat dx=(cameraPoint.x-cameraStart.x)/(55*scale),dy=(cameraStart.y-cameraPoint.y)/(55*scale); a[2]=fmax(-1,fmin(1,dx))*32767; a[3]=fmax(-1,fmin(1,dy))*32767; }
    pthread_mutex_lock(&mu); buttons=m; memcpy(axes,a,sizeof a); pthread_mutex_unlock(&mu);
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for(UITouch *t in touches) { CGPoint p=[t locationInView:self]; int best=-1; CGFloat bd=1;
        for(int i=0;i<count;i++){ Key k=keys[i]; CGFloat d=hypot(p.x-k.x,p.y-k.y)/MAX(k.r*1.2,22); if(d<bd)best=i,bd=d; }
        if(best>=0){ if(keys[best].mask==0){ pthread_mutex_lock(&mu); menuRequested=1; pthread_mutex_unlock(&mu); } else [owners setObject:@(best) forKey:t]; continue;}
        CGPoint c=CGPointMake(leftEdge+66*scale,bottomEdge-77*scale);
        if(!leftTouch && hypot(p.x-c.x,p.y-c.y)<65*scale){leftTouch=t;leftPoint=p;[owners setObject:@(-1) forKey:t];}
        else if(!cameraTouch && p.x>width*0.45){cameraTouch=t;cameraStart=cameraPoint=p;[owners setObject:@(-2) forKey:t];}
    }
    [self publish]; [self setNeedsDisplay];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; for(UITouch *t in touches){if(t==leftTouch)leftPoint=[t locationInView:self]; if(t==cameraTouch){ cameraPoint=[t locationInView:self]; CGFloat dx=cameraPoint.x-cameraStart.x,dy=cameraPoint.y-cameraStart.y,d=hypot(dx,dy),r=55*scale; if(d>r) cameraStart=CGPointMake(cameraPoint.x-dx*r/d,cameraPoint.y-dy*r/d); }} [self publish]; [self setNeedsDisplay]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; for(UITouch *t in touches){if(t==leftTouch)leftTouch=nil; if(t==cameraTouch)cameraTouch=nil; [owners removeObjectForKey:t];} [self publish]; [self setNeedsDisplay]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)event {
    if (overlay_menu_open() && count>0) { Key k=keys[count-1]; if(hypot(p.x-k.x,p.y-k.y)>MAX(k.r*1.2,22)) return nil; }
    return [super hitTest:p withEvent:event];
}
@end

void hunter_touch_attach(SDL_Window *window) {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIWindow *w=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
        if(w) {
            // SDL's UIKit controller observes every keyboard, including our native
            // alert, and moves its rendering view on keyboard notifications. The
            // app uses the native alert instead of SDL's keyboard; leave that view
            // alone when the alert opens or closes.
            UIViewController *vc=w.rootViewController;
            NSNotificationCenter *nc=NSNotificationCenter.defaultCenter;
            [nc removeObserver:vc name:UIKeyboardWillShowNotification object:nil];
            [nc removeObserver:vc name:UIKeyboardWillHideNotification object:nil];
            HunterPad *v=[[HunterPad alloc] initWithFrame:w.bounds]; [w addSubview:v]; activePad = v;
            [v setPhysicalControllerConnected:atomic_load(&controllerConnected) != 0];
        }
    });
}
void hunter_touch_set_controller_connected(int connected) {
    int state = connected != 0;
    if (atomic_exchange(&controllerConnected, state) == state) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [activePad setPhysicalControllerConnected:atomic_load(&controllerConnected) != 0];
    });
}
void hunter_touch_read(uint64_t *b,int *lx,int *ly,int *rx,int *ry) {
    pthread_mutex_lock(&mu); *b=buttons; *lx=axes[0]; *ly=axes[1]; *rx=axes[2]; *ry=axes[3]; pthread_mutex_unlock(&mu);
}
int hunter_touch_take_menu(void) { pthread_mutex_lock(&mu); int v=menuRequested; menuRequested=0; pthread_mutex_unlock(&mu); return v; }
