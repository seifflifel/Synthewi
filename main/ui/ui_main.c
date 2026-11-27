// MAIN screen — 6-card overview + single-card edit (draw only).

#include <string.h>
#include <stdio.h>
#include "ui_internal.h"

// Returns fraction string for echo delay (0-10000 scale, snapped to nearest 1/x of a second).
static const char* ui_echo_frac(uint16_t v) {
    if (v <  978) return "1/16";
    if (v < 2133) return "1/8";
    if (v < 3511) return "1/6";
    if (v < 5378) return "1/4";
    if (v < 8156) return "1/3";
    return "1/2";
}

// ---- MAIN card — 6-card overview + per-card edit ----------------

// Thin vertical bar: 1px track edges, fill from bottom.
static void ui_vbar(int x0, int x1, int y_top, int y_bot, uint16_t val, uint16_t col)
{
    int fh = (int)((long)val * (y_bot - y_top) / 10000);
    tft_fill((uint8_t)x0, (uint8_t)y_top, (uint8_t)x1, (uint8_t)y_bot, C_BLACK);
    tft_fill((uint8_t)x0, (uint8_t)y_top, (uint8_t)x0, (uint8_t)y_bot, C_DKGRAY);
    tft_fill((uint8_t)x1, (uint8_t)y_top, (uint8_t)x1, (uint8_t)y_bot, C_DKGRAY);
    if (fh > 0) tft_fill((uint8_t)x0, (uint8_t)(y_bot-fh), (uint8_t)x1, (uint8_t)y_bot, col);
}

// ---- 6-card overview ----------------------------------------
static void ui_draw_main_overview(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    // 1px dividers between cols and rows
    tft_fill(52,  22, 52,  TFT_H-1, C_DKGRAY);
    tft_fill(105, 22, 105, TFT_H-1, C_DKGRAY);
    tft_fill(0,   74, TFT_W-1, 74,  C_DKGRAY);

    static const int    cxs[3]  = {0, 53, 106};
    static const int    cws[3]  = {52, 52, 53};
    static const int    rys[2]  = {22, 75};
    static const int    rhs[2]  = {52, 53};
    static const char * const nm[6] = {"ENV","FLT","EG1","LFO","FX","GLD"};
    static const char * const flt_nm[3] = {"LPF","BPF","HPF"};

    amy_engine_state_t st;
    amy_engine_get_state(&st);

    for (int ci = 0; ci < 6; ci++) {
        int col = ci % 3, row = ci / 3;
        int cx = cxs[col], cw = cws[col];
        int ry = rys[row], rh = rhs[row];
        bool sel = (ci == ui_main_card);
        uint16_t oc = sel ? C_ORANGE : C_DKGRAY;

        tft_rrect(cx, ry, cx+cw-1, ry+rh-1, oc);
        // Header (10px): filled orange when selected
        tft_fill(cx+1, ry+1, cx+cw-2, ry+9, sel ? C_ORANGE : C_BLACK);
        int tx = cx + (cw - (int)strlen(nm[ci])*6) / 2;
        tft_text(nm[ci], tx, ry+1, sel ? C_BLACK : C_DKGRAY, sel ? C_ORANGE : C_BLACK, 1);
        tft_fill(cx+1, ry+10, cx+cw-2, ry+10, oc);

        int bx = cx + 3;
        int by = ry + 13;  // body start y

        switch (ci) {
        case 0: { // ENV — mini ADSR shape
            int ex0=cx+3, ex1=cx+cw-4;
            int ey_t=ry+15, ey_b=ry+rh-5, eh=ey_b-ey_t;
            int Aw=(int)((long)st.env_attack *(ex1-ex0)/30000); if(Aw<1)Aw=1;
            int Dw=(int)((long)st.env_decay  *(ex1-ex0)/30000); if(Dw<1)Dw=1;
            int Sw=3;
            int Rw=(int)((long)st.env_release*(ex1-ex0)/30000); if(Rw<1)Rw=1;
            int tot=Aw+Dw+Sw+Rw, av=ex1-ex0-2;
            if(tot>av){Aw=Aw*av/tot;Dw=Dw*av/tot;Rw=Rw*av/tot;Sw=2;}
            int sy=ey_b-(int)((long)st.env_sustain*eh/10000);
            if(sy<ey_t+1)sy=ey_t+1;
            int xe=ex0+(av-(Aw+Dw+Sw+Rw))/2;
            int xa=xe+Aw,xd=xa+Dw,xs=xd+Sw,xr=xs+Rw;
            tft_line(xe,ey_b,xa,ey_t,oc); tft_line(xa,ey_t,xd,sy,oc);
            tft_line(xd,sy,xs,sy,oc);     tft_line(xs,sy,xr,ey_b,oc);
            break;
        }
        case 1: { // FLT — type + F + Q
            tft_text(flt_nm[st.filter_type], bx, by, oc, C_BLACK, 1);
            float fhz = ui_filter_hz(st.filter_cutoff);
            char buf[24];
            if(fhz>=1000.0f) snprintf(buf,sizeof(buf),"F%3uK",(unsigned)(fhz/1000.0f+0.5f));
            else              snprintf(buf,sizeof(buf),"F%4u",(unsigned)fhz);
            tft_text(buf, bx, by+9,  oc, C_BLACK, 1);
            float fq=ui_filter_q(st.filter_resonance);
            uint32_t qi=(uint32_t)(fq*10.0f);
            snprintf(buf,sizeof(buf),"Q%u.%u",(unsigned)(qi/10),(unsigned)(qi%10));
            tft_text(buf, bx, by+18, oc, C_BLACK, 1);
            break;
        }
        case 2: { // EG1 — depth + decay
            char buf[24];
            { unsigned t = ui_fenv_oct10(st.filter_env_depth);
              snprintf(buf,sizeof(buf),"F+%u.%uOCT",t/10,t%10); }
            tft_text(buf, bx, by,   oc, C_BLACK, 1);
            snprintf(buf,sizeof(buf),"D%4u",(unsigned)ui_eg1_decay_ms(st.filter_env_decay));
            tft_text(buf, bx, by+9, oc, C_BLACK, 1);
            break;
        }
        case 3: { // LFO — rate + depth
            float rh2=ui_lfo_hz(st.lfo_rate);
            uint32_t rh10=(uint32_t)(rh2*10.0f);
            char buf[24];
            snprintf(buf,sizeof(buf),"R%u.%uHZ",(unsigned)(rh10/10),(unsigned)(rh10%10));
            tft_text(buf, bx, by,   oc, C_BLACK, 1);
            { unsigned t = ui_lfo_oct10(st.lfo_depth);
              snprintf(buf,sizeof(buf),"D%u.%uOCT",t/10,t%10); }
            tft_text(buf, bx, by+9, oc, C_BLACK, 1);
            break;
        }
        case 4: { // FX — echo amount + delay fraction, reverb amount
            char buf[24];
            snprintf(buf,sizeof(buf),"E%3u%%",(unsigned)(st.echo_amount/100));
            tft_text(buf, bx, by,    oc, C_BLACK, 1);
            tft_text(ui_echo_frac(st.echo_delay), bx, by+9, oc, C_BLACK, 1);
            snprintf(buf,sizeof(buf),"R%3u%%",(unsigned)(st.reverb_amount/100));
            tft_text(buf, bx, by+18, oc, C_BLACK, 1);
            break;
        }
        case 5: { // GLD — ms value
            uint32_t ms=ui_glide_ms(st.glide);
            char buf[24];
            if(ms==0) snprintf(buf,sizeof(buf),"OFF");
            else      snprintf(buf,sizeof(buf),"%3uMS",(unsigned)ms);
            int tw=(int)strlen(buf)*6;
            tft_text(buf, cx+(cw-tw)/2, by+9, oc, C_BLACK, 1);
            break;
        }
        }
    }
}

// ---- Single-card edit ----------------------------------------
static void ui_draw_main_card_edit(void)
{
    tft_fill(0, 22, TFT_W-1, TFT_H-1, C_BLACK);

    static const char * const nm[6]       = {"ENV","FLT","EG1","LFO","FX","GLD"};
    static const char * const flt_nm[3]   = {"LPF","BPF","HPF"};

    tft_text_bold(nm[ui_main_card], 4, 24, C_ORANGE, C_BLACK, 2);
    tft_fill(0, 42, TFT_W-1, 43, C_LTGRAY);

    int p  = ui_main_param;
    // ENV needs 4 param lines so visual is shorter; others get more visual space.
    int vt = 46;
    // FX needs 5 lines (79..118, just above the footer at 119).
    int vb = (ui_main_card == 0) ? 80 : (ui_main_card == 4) ? 70 : 88;
    int py = (ui_main_card == 0) ? 83 : (ui_main_card == 4) ? 79 : 91;

    // PC(i): blue if focused param, orange otherwise (bars, labels, line text)
    // PF/PB(i): param LINE colours — while editing, the focused line is inverted
    //           (white on blue) so it is obvious that rotating changes the value.
    #define PC(i) ((p==(i)) ? C_BLUE : C_ORANGE)
    #define PEDIT(i) ((p==(i)) && ui_main_editing)
    #define PF(i) (PEDIT(i) ? C_BLACK : PC(i))
    #define PB(i) (PEDIT(i) ? C_BLUE  : C_BLACK)

    switch (ui_main_card) {
    case 0: { // ENV — ADSR envelope shape + 4 param lines
        int ex0=8, ex1=TFT_W-9, eh=vb-vt;
        int Aw=(int)((long)ui_main_adsr[0]*(ex1-ex0)/30000); if(Aw<1)Aw=1;
        int Dw=(int)((long)ui_main_adsr[1]*(ex1-ex0)/30000); if(Dw<1)Dw=1;
        int Sw=5;
        int Rw=(int)((long)ui_main_adsr[3]*(ex1-ex0)/30000); if(Rw<1)Rw=1;
        int tot=Aw+Dw+Sw+Rw, av=ex1-ex0-2;
        if(tot>av){Aw=Aw*av/tot;Dw=Dw*av/tot;Rw=Rw*av/tot;Sw=4;}
        int sy=vb-(int)((long)ui_main_adsr[2]*eh/10000);
        if(sy<vt+2)sy=vt+2;
        int xa=ex0+Aw,xd=xa+Dw,xs=xd+Sw,xr=xs+Rw;
        tft_line(ex0,vb,xa,vt,PC(0)); tft_line(xa,vt,xd,sy,PC(1));
        tft_line(xd,sy,xs,sy,PC(2)); tft_line(xs,sy,xr,vb,PC(3));
        char buf[24];
        snprintf(buf,sizeof(buf),"A  %4uMS",(unsigned)ui_a_ms(ui_main_adsr[0]));
        tft_text(buf,4,py,PF(0),PB(0),1);
        snprintf(buf,sizeof(buf),"D  %4uMS",(unsigned)ui_d_ms(ui_main_adsr[1]));
        tft_text(buf,4,py+8,PF(1),PB(1),1);
        snprintf(buf,sizeof(buf),"S    %3u%%",(unsigned)ui_s_pct(ui_main_adsr[2]));
        tft_text(buf,4,py+16,PF(2),PB(2),1);
        snprintf(buf,sizeof(buf),"R  %4uMS",(unsigned)ui_r_ms(ui_main_adsr[3]));
        tft_text(buf,4,py+24,PF(3),PB(3),1);
        break;
    }
    case 1: { // FLT — type label + two bars + 3 param lines
        tft_text(flt_nm[ui_main_flt_type], 4, vt, PC(2), C_BLACK, 1);
        int bw=5,gap=12;
        int bx0=(TFT_W-(bw+gap+bw))/2, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt+12,vb,ui_main_flt_cut,PC(0));
        ui_vbar(bx1,bx1+bw-1,vt+12,vb,ui_main_flt_res,PC(1));
        tft_text("F",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("Q",bx1+1,vb+2,PC(1),C_BLACK,1);
        char buf[24];
        float fhz=ui_filter_hz(ui_main_flt_cut);
        if(fhz>=1000.0f) snprintf(buf,sizeof(buf),"F  %3uKHZ",(unsigned)(fhz/1000.0f+0.5f));
        else              snprintf(buf,sizeof(buf),"F  %4uHZ", (unsigned)fhz);
        tft_text(buf,4,py,PF(0),PB(0),1);
        float fq=ui_filter_q(ui_main_flt_res);
        uint32_t qi=(uint32_t)(fq*10.0f);
        snprintf(buf,sizeof(buf),"Q  %u.%u",(unsigned)(qi/10),(unsigned)(qi%10));
        tft_text(buf,4,py+8,PF(1),PB(1),1);
        snprintf(buf,sizeof(buf),"TYPE  %s",flt_nm[ui_main_flt_type]);
        tft_text(buf,4,py+16,PF(2),PB(2),1);
        break;
    }
    case 2: { // EG1 — two bars + 2 param lines
        int bw=5,gap=14;
        int bx0=(TFT_W-(bw+gap+bw))/2, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt,vb,ui_main_eg1_depth,PC(0));
        ui_vbar(bx1,bx1+bw-1,vt,vb,ui_main_eg1_decay, PC(1));
        tft_text("F",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("D",bx1+1,vb+2,PC(1),C_BLACK,1);
        char buf[24];
        { unsigned t = ui_fenv_oct10(ui_main_eg1_depth);
          snprintf(buf,sizeof(buf),"F  +%u.%u OCT",t/10,t%10); }
        tft_text(buf,4,py,PF(0),PB(0),1);
        snprintf(buf,sizeof(buf),"D  %4uMS",(unsigned)ui_eg1_decay_ms(ui_main_eg1_decay));
        tft_text(buf,4,py+8,PF(1),PB(1),1);
        break;
    }
    case 3: { // LFO — two bars + 2 param lines
        int bw=5,gap=14;
        int bx0=(TFT_W-(bw+gap+bw))/2, bx1=bx0+bw+gap;
        ui_vbar(bx0,bx0+bw-1,vt,vb,ui_main_lfo_rate, PC(0));
        ui_vbar(bx1,bx1+bw-1,vt,vb,ui_main_lfo_depth,PC(1));
        tft_text("R",bx0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("D",bx1+1,vb+2,PC(1),C_BLACK,1);
        char buf[24];
        float rh=ui_lfo_hz(ui_main_lfo_rate);
        uint8_t rh_i=(uint8_t)(rh>99.0f?99.0f:rh);
        uint8_t rh_f=(uint8_t)((unsigned)(rh*10.0f)%10);
        snprintf(buf,sizeof(buf),"R  %u.%uHZ",rh_i,rh_f);
        tft_text(buf,4,py,PF(0),PB(0),1);
        { unsigned t = ui_lfo_oct10(ui_main_lfo_depth);
          snprintf(buf,sizeof(buf),"D  %u.%u OCT",t/10,t%10); }
        tft_text(buf,4,py+8,PF(1),PB(1),1);
        break;
    }
    case 4: { // FX — echo (AMT/FB/DLY) + reverb (REV/SIZE): 4 bars + 5 param lines
        // Bars sit on the right half; their labels share the first text row
        // but not its x range (text lines end at x~64).
        int bw=5, gap=9;
        int ex0=78, ex1=ex0+bw+gap;           // echo pair
        int rx0=ex1+bw+gap+8, rx1=rx0+bw+gap; // reverb pair
        ui_vbar(ex0,ex0+bw-1,vt,vb,ui_main_ech_amt,PC(0));
        ui_vbar(ex1,ex1+bw-1,vt,vb,ui_main_ech_fb, PC(1));
        ui_vbar(rx0,rx0+bw-1,vt,vb,ui_main_rev_amt,PC(3));
        ui_vbar(rx1,rx1+bw-1,vt,vb,ui_main_rev_dec,PC(4));
        tft_text("A",ex0+1,vb+2,PC(0),C_BLACK,1);
        tft_text("F",ex1+1,vb+2,PC(1),C_BLACK,1);
        tft_text("R",rx0+1,vb+2,PC(3),C_BLACK,1);
        tft_text("S",rx1+1,vb+2,PC(4),C_BLACK,1);
        char buf[24];
        snprintf(buf,sizeof(buf),"ECHO %3u%%",(unsigned)(ui_main_ech_amt/100));
        tft_text(buf,4,py,PF(0),PB(0),1);
        snprintf(buf,sizeof(buf),"FB   %3u%%",(unsigned)(ui_main_ech_fb/100));
        tft_text(buf,4,py+8,PF(1),PB(1),1);
        snprintf(buf,sizeof(buf),"DLY  %s",ui_echo_frac(ui_main_ech_dly));
        tft_text(buf,4,py+16,PF(2),PB(2),1);
        snprintf(buf,sizeof(buf),"REV  %3u%%",(unsigned)(ui_main_rev_amt/100));
        tft_text(buf,4,py+24,PF(3),PB(3),1);
        snprintf(buf,sizeof(buf),"SIZE %3u%%",(unsigned)(ui_main_rev_dec/100));
        tft_text(buf,4,py+32,PF(4),PB(4),1);
        break;
    }
    case 5: { // GLD — single bar + 1 param line
        int bx=TFT_W/2-3;
        ui_vbar(bx,bx+5,vt,vb,ui_main_gld,PC(0));
        tft_text("G",bx+1,vb+2,PC(0),C_BLACK,1);
        uint16_t ms=(uint16_t)ui_glide_ms(ui_main_gld);
        char buf[24];
        if(ms==0) snprintf(buf,sizeof(buf),"GLD  OFF");
        else      snprintf(buf,sizeof(buf),"GLD  %3uMS",ms);
        tft_text(buf,4,py,PF(0),PB(0),1);
        break;
    }
    }
    #undef PC
    #undef PEDIT
    #undef PF
    #undef PB

    tft_fill(0, 119, TFT_W-1, 120, C_DKGRAY);
    tft_text(ui_main_editing ? "ROT:ADJ  BTN:OK    HLD:BACK"
                             : "ROT:SEL  BTN:EDIT  HLD:BACK", 2, 122, C_DKGRAY, C_BLACK, 1);
}

void ui_draw_main(void)
{
    if (ui_main_in_card) ui_draw_main_card_edit();
    else                 ui_draw_main_overview();
}
