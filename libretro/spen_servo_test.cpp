// Native unit test for the S-Pen servo. Mirrors tools/spen-sim/validate.cjs.
// Build: g++ -std=c++11 spen_servo_test.cpp -o spen_servo_test && ./spen_servo_test
#include "spen_servo.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

static const int W = 256, H = 224;
static int failures = 0;
static void check(const char* name, bool cond, double v) {
    printf("  %s  %s (%.2f)\n", cond ? "PASS" : "FAIL", name, v);
    if (!cond) failures++;
}

// Replicates snes9x UpdatePolledMouse: emit clamp(cur-old,+-127), carry remainder.
struct MouseCh { int cur=0, old=0; int step(int s){ cur+=s; int j=cur-old, e;
    if(j<-127){e=-127;old-=127;} else if(j<0){e=j;old=cur;}
    else if(j>127){e=127;old+=127;} else {e=j;old=cur;} return e; } };

struct Sim {
    spen_servo_params p; double est_x, est_y, true_x, true_y; MouseCh mx, my; double St;
    Sim(double kp,double gain,int dz,double st){ p.kp=kp;p.gain=gain;p.deadzone=dz;p.clamp=127;
        est_x=est_y=W/2.0; true_x=true_y=H/2.0; St=st; }
    void reset(){ est_x=W/2.0; est_y=H/2.0; true_x=W/2.0; true_y=H/2.0; mx=MouseCh(); my=MouseCh(); }
    void stepTo(double tx,double ty){
        int sx=spen_servo_step(tx,&est_x,&p); int sy=spen_servo_step(ty,&est_y,&p);
        true_x += mx.step(sx)*St; true_y += my.step(sy)*St;
        true_x=std::max(0.0,std::min((double)W-1,true_x)); true_y=std::max(0.0,std::min((double)H-1,true_y));
    }
    double drift(double px,double py){ return std::hypot(true_x-px, true_y-py); }
};

int main(){
    { Sim s(1.0,1.0,2,1.0); s.reset();
      double tx=8, ty=8; for(int f=0;f<90;f++) s.stepTo(tx,ty);
      check("corners-ish: lands under pen (<=3px)", s.drift(tx,ty)<=3.0, s.drift(tx,ty)); }

    { Sim s(1.0,1.0,2,1.0); s.reset();
      for(int f=0;f<200;f++) s.stepTo(60,180); double d1=s.drift(60,180);
      for(int f=0;f<2000;f++) s.stepTo(60,180); double d2=s.drift(60,180);
      check("no accumulation over time (<0.01px)", std::fabs(d2-d1)<0.01, std::fabs(d2-d1)); }

    { Sim s(1.0,1.0,2,1.0); s.reset(); double maxLag=0,x=40;
      for(int f=0;f<25;f++){ x+=8; s.stepTo(x,112); maxLag=std::max(maxLag,s.drift(x,112)); }
      check("Kp=1.0 zero-lag tracking (<=1px)", maxLag<=1.0, maxLag); }

    { Sim s(1.0,1.0,2,2.0); s.reset();
      for(int f=0;f<90;f++) s.stepTo(8,8);
      check("gain mismatch bounded (<256px)", s.drift(8,8)<256.0, s.drift(8,8)); }

    printf("\n%s\n", failures==0 ? "ALL PASS" : "FAILURES");
    return failures==0 ? 0 : 1;
}
