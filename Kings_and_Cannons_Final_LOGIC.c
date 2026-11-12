/*DONT RUN THIS; ITS JUST FOR LOGIC HEHE*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

/* KINGS AND CANNONS - local two-player Linux terminal game.
   Mouse: choose a toolbar item, then click its board destination.
   Keyboard: E or Enter ends turn, Q quits.
   Back cancels selection or undoes the most recent action this turn.
*/
#define N 10
#define MAX_SHIELDS 10
#define MAX_CANNONS 2
#define TURN_SECONDS 60
#define HISTORY_MAX 160
#define BX 5
#define BY 10
#define BW 8
#define BH 4
#define XEND (BX + N*BW)
#define RED_BG "\033[48;2;255;207;207m\033[30m"
#define GREEN_BG "\033[48;2;205;255;214m\033[30m"
#define GOLD_BG "\033[48;2;255;229;121m\033[30m"
#define BLACK_BG "\033[48;2;17;19;23m\033[97m"
#define SHOT_BG "\033[48;2;255;155;40m\033[30m"
#define BOOM_BG "\033[48;2;255;81;81m\033[97m"
#define SELECT_BG "\033[48;2;91;155;255m\033[97m"
#define PANEL "\033[48;2;50;55;66m\033[97m"
#define RESET "\033[0m"
#define STRONG "\033[1m"

typedef struct { int r,c; } Pos;
typedef enum { EMPTY,KING,SHIELD,CANNON } Piece;
typedef struct { Piece piece; int owner; } Cell;
typedef struct { int r,c,dir,owner,alive; } Shot;
typedef struct {
    Cell cells[N][N];
    unsigned char crater[N][N], breach[N][N];
    Pos king[2];
    int lives[2], shields_placed[2], shields_hit[2], nuked[2];
    int movement[2][6];
    Pos nuke_target[2];
    int cannons_this_round[2];
} Game;

static Game g;
static Game history[HISTORY_MAX];
static int history_len = 0;
static int current = 0, round_no = 1;
static int selection = 0, move_type = 1; /* 0 none; 1 shield; 2 cannon; 3 move; 4 nuke */
static int game_over = 0, victor = -1; /* -1 none, 0/1 winner, 2 draw */
static char notice[240] = "Click a square to place Player 1's KING.";
static struct termios old_tty;
static int tty_ready = 0;
static double deadline = 0.0;

static int within(int r,int c){return r>=0&&r<N&&c>=0&&c<N;}
static int own_side(int p,int r){return p==0?r<5:r>=5;}
static int absi(int v){return v<0?-v:v;}
static int signi(int v){return (v>0)-(v<0);}
static double mono(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void delay_ms(int ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000L};while(nanosleep(&t,&t)<0&&errno==EINTR){}}
static void goto_xy(int y,int x){printf("\033[%d;%dH",y,x);}

static void restore_terminal(void){
    fputs("\033[?1000l\033[?1006l\033[?25h\033[0m\n",stdout);
    fflush(stdout);
    if(tty_ready){tcsetattr(STDIN_FILENO,TCSANOW,&old_tty);tty_ready=0;}
}
static void interrupted(int sig){
    fputs("\033[?1000l\033[?1006l\033[?25h\033[0m",STDOUT_FILENO==1?stdout:stderr);
    if(tty_ready)tcsetattr(STDIN_FILENO,TCSANOW,&old_tty);
    _exit(128+sig);
}
static int setup_terminal(void){
    if(tcgetattr(STDIN_FILENO,&old_tty)!=0)return 0;
    struct termios raw=old_tty;
    raw.c_lflag &= (tcflag_t)~(ICANON|ECHO);
    raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&raw)!=0)return 0;
    tty_ready=1;
    fputs("\033[?1000h\033[?1006h\033[?25l",stdout);
    fflush(stdout);
    return 1;
}

static void draw_toolbar_box(int x,int w,const char *top,const char *bottom,int selected){
    goto_xy(5,x);printf("%s+",selected?SELECT_BG:PANEL);
    for(int z=0;z<w-2;z++)putchar('-');
    printf("+%s",RESET);
    goto_xy(6,x);printf("%s| %-*.*s|%s",selected?SELECT_BG:PANEL,w-3,w-3,top,RESET);
    goto_xy(7,x);printf("%s| %-*.*s|%s",selected?SELECT_BG:PANEL,w-3,w-3,bottom,RESET);
    goto_xy(8,x);printf("%s+",selected?SELECT_BG:PANEL);
    for(int z=0;z<w-2;z++)putchar('-');
    printf("+%s",RESET);
}

static int available_moves(int p,int type){return type==1?999:g.movement[p][type];}
static int is_free(int r,int c){return within(r,c)&&g.cells[r][c].piece==EMPTY;}
static int can_move(int p,int type,int dest_r,int dest_c,int *damage){
    Pos from=g.king[p];int dr=absi(dest_r-from.r),dc=absi(dest_c-from.c),ok=0;
    if(!within(dest_r,dest_c)||!own_side(p,dest_r)||!is_free(dest_r,dest_c))return 0;
    if(!available_moves(p,type)||(dr==0&&dc==0))return 0;
    switch(type){
        case 1:ok=(dr+dc==1);break;
        case 2:ok=(dr==2&&dc==2);break;
        case 3:ok=(dr==2&&dc==1)||(dr==1&&dc==2);break;
        case 4:ok=(dr==2&&dc==0)||(dr==0&&dc==2);break;
        case 5:ok=(dr==dc||dr==0||dc==0);break;
    }
    if(!ok)return 0;
    int hits=0;
    if(type!=3){
        int rs=signi(dest_r-from.r),cs=signi(dest_c-from.c);
        for(int r=from.r+rs,c=from.c+cs;r!=dest_r||c!=dest_c;r+=rs,c+=cs){
            if(g.cells[r][c].piece!=EMPTY)return 0;
            if(g.crater[r][c])hits++;
        }
    }
    if(g.crater[dest_r][dest_c])hits++;
    if(damage)*damage=hits;
    return 1;
}

static void draw_grid(int *shots,int nshots,int *booms,int nbooms){
    goto_xy(BY,1);printf("    \033[38;5;252m+");
    for(int c=0;c<N;c++)printf("-------+");
        printf(RESET);
    for(int r=0;r<N;r++){
        for(int sub=0;sub<3;sub++){
            goto_xy(BY+1+4*r+sub,1);
            if(sub==1)printf("%2d  ",r+1);else printf("    ");
            printf("\033[38;5;252m|" RESET);
            for(int c=0;c<N;c++){
                int nuke_flash=0,bullet=0;
                for(int i=0;i<nshots;i++)if(shots[i]==r*N+c)bullet=1;
                for(int i=0;i<nbooms;i++)if(booms[i]==r*N+c)nuke_flash=1;
                int damage=0;
                int legal_dest = (selection==3&&g.lives[current]>0&&can_move(current,move_type,r,c,&damage));
                const char *bg=r<5?RED_BG:GREEN_BG;
                if(g.crater[r][c]||g.breach[r][c])bg=BLACK_BG;
                if(legal_dest)bg=damage?BOOM_BG:GOLD_BG;
                if(bullet)bg=SHOT_BG;
                if(nuke_flash)bg=BOOM_BG;
                char text[8]="       ";
                if(sub==1){
                    Cell cell=g.cells[r][c];
                    if(cell.piece==KING)strcpy(text,cell.owner==0?"  K1   ":"  K2   ");
                    else if(cell.piece==SHIELD)strcpy(text," [SH]  ");
                    else if(cell.piece==CANNON)strcpy(text,cell.owner==0?"  C1   ":"  C2   ");
                    else if(g.crater[r][c])strcpy(text,"  XXX  ");
                    else if(g.breach[r][c])strcpy(text,"  ...  ");
                    else if(legal_dest)strcpy(text,damage?"   !   ":"   *   ");
                    if(bullet)strcpy(text,"  |!|  ");
                    if(nuke_flash)strcpy(text,"  BOOM ");
                }
                printf("%s%s%s\033[38;5;252m|%s",bg,text,RESET,RESET);
            }
        }
        goto_xy(BY+4+4*r,1);printf("    \033[38;5;252m+");
        for(int c=0;c<N;c++)printf("-------+");
        printf(RESET);
    }
    fflush(stdout);
}

static void draw_inventory(void){
    goto_xy(51,1);printf("\033[2K" PANEL "  PLAYER INVENTORIES (independent resources)                                    " RESET);
    for(int p=0;p<2;p++){
        goto_xy(52+p,1);
        printf("\033[2K%s%s P%d %s  Lives %d/3 | Shields %d/10 | Cannons %d/2 | Nuke %s (%d/3 hits) %s",
            p==current?STRONG:"",p==current?" >":"  ",p+1,
            p==0?"RED  ":"GREEN",g.lives[p],MAX_SHIELDS-g.shields_placed[p],
            MAX_CANNONS-g.cannons_this_round[p],
            g.nuked[p]?"USED":g.shields_hit[p]>=3?"READY":"LOCKED",
            g.shields_hit[p],RESET);
    }
    goto_xy(54,1);printf("\033[2K  ACTIVE PLAYER %d | Move charges: Step unlimited, Skew %d, Leap %d, Axis %d, Jasmin %d",
        current+1,g.movement[current][2],g.movement[current][3],
        g.movement[current][4],g.movement[current][5]);
    goto_xy(56,1);printf("\033[2K" PANEL "  KING MOVEMENTS - click to choose and see legal squares                            " RESET);
    for(int type=1;type<=5;type++){
        int x=1+(type-1)*16;
        char title[24],bottom[24];
        const char *names[]={"","STEP","SKEW","LEAP","AXIS","JASMIN"};
        snprintf(title,sizeof title,"%s",names[type]);
        if(type==1)snprintf(bottom,sizeof bottom,"Unlimited");
        else snprintf(bottom,sizeof bottom,"%d left",g.movement[current][type]);
        goto_xy(57,x);printf("%s[%-12s]%s",selection==3&&type==move_type?SELECT_BG:PANEL,title,RESET);
        goto_xy(58,x);printf("%s %-13s %s",selection==3&&type==move_type?SELECT_BG:PANEL,bottom,RESET);
    }
    goto_xy(60,1);printf("\033[2K  %s",notice);
    goto_xy(61,1);printf("\033[2K  %s",game_over?"Q: quit":"Click action, then board. BACK: undo | CANCEL: deselect | E/Enter: END TURN | Q: quit");
    fflush(stdout);
}
static void draw_header(void){
    goto_xy(1,1);printf("\033[2K%s KINGS AND CANNONS %s                 ROUND %02d",STRONG,RESET,round_no);
    goto_xy(2,1);printf("\033[2K%s  %s PLAYER %d TURN %s  %s",current==0?"\033[41;97m":"\033[42;30m",STRONG,current+1,RESET,current==0?"RED / ROWS 1-5":"GREEN / ROWS 6-10");
    goto_xy(3,1);printf("\033[2K  TIME LEFT: %02d sec   |   %s",(int)(deadline>0?((deadline-mono()>0)?deadline-mono()+0.999:0):0),game_over?"GAME OVER":"Place any number of pieces until time ends or you end your turn");
    draw_toolbar_box(1,17,"SHIELD","10 each",selection==1);
    draw_toolbar_box(18,17,"CANNON","2 / round",selection==2);
    draw_toolbar_box(35,17,"MOVE KING","Choose move",selection==3);
    draw_toolbar_box(52,13,"NUKE","3 hits",selection==4);
    draw_toolbar_box(65,11,"BACK","Undo",0);
    draw_toolbar_box(76,10,"CANCEL","Select",0);
    goto_xy(9,1);printf("      ");for(int c=0;c<N;c++)printf("   %c    ",'A'+c);
}
static void redraw(void){
    fputs("\033[2J\033[H",stdout);
    draw_header();draw_grid(NULL,0,NULL,0);draw_inventory();
    fflush(stdout);
}
static void tick(void){
    goto_xy(3,1);printf("\033[2K  TIME LEFT: %02d sec   |   %s",(int)(deadline>0?((deadline-mono()>0)?deadline-mono()+0.999:0):0),game_over?"GAME OVER":"Click items, place them, then END TURN with E / ENTER");
    fflush(stdout);
}
static void save_undo(void){if(history_len<HISTORY_MAX)history[history_len++]=g;}
static void undo(void){
    if(history_len>0){g=history[--history_len];snprintf(notice,sizeof notice,"Undid your most recent action. Timer continues.");}
    else snprintf(notice,sizeof notice,"Nothing to undo this turn.");
    selection=0;redraw();
}
static void select_tool(int value){
    if(value==1&&g.shields_placed[current]>=MAX_SHIELDS){snprintf(notice,sizeof notice,"No shields remaining (10 per player).");return;}
    if(value==2&&g.cannons_this_round[current]>=MAX_CANNONS){snprintf(notice,sizeof notice,"No cannons left this round (2 per player).");return;}
    if(value==4&&(g.shields_hit[current]<3||g.nuked[current])){snprintf(notice,sizeof notice,"NUKE: destroy 3 shields first; only once per player.");return;}
    selection=value;
    if(value==3)snprintf(notice,sizeof notice,"MOVE KING: click a movement type, then a highlighted square. Yellow = safe; red ! = crater damage.");
    else snprintf(notice,sizeof notice,"%s selected. Click an empty square on your side%s.",value==1?"SHIELD":value==2?"CANNON":"NUKE",value==4?" (nuke can target anywhere)":"");
}
static void finish_king_loss(int p){
    if(g.lives[p]>0)return;
    g.lives[p]=0;
    game_over=1;
    victor=1-p;
    snprintf(notice,sizeof notice,"PLAYER %d KING lost all 3 lives! PLAYER %d WINS!",p+1,2-p);
}
static void select_square(int r,int c){
    if(!within(r,c))return;
    if(selection==0){snprintf(notice,sizeof notice,"Select SHIELD, CANNON, MOVE KING or NUKE first.");return;}
    if(selection==4){
        if(g.nuked[current]||g.shields_hit[current]<3)return;
        save_undo();g.nuked[current]=1;g.nuke_target[current]=(Pos){r,c};
        snprintf(notice,sizeof notice,"NUKE aimed at %c%d. It will explode during firing; BACK to undo.",'A'+c,r+1);
    }else if(selection==3){
        int damage=0;if(!can_move(current,move_type,r,c,&damage)){
            snprintf(notice,sizeof notice,"Invalid movement. Only highlighted squares are legal.");return;
        }
        save_undo();
        Pos start=g.king[current];
        g.cells[start.r][start.c]=(Cell){EMPTY,-1};
        g.cells[r][c]=(Cell){KING,current};g.king[current]=(Pos){r,c};
        if(move_type!=1)g.movement[current][move_type]--;
        if(damage>0){g.lives[current]-=damage;finish_king_loss(current);}
        if(!game_over)snprintf(notice,sizeof notice,"King moved to %c%d using movement %d. %s",'A'+c,r+1,move_type,damage?"Crater cost king life!":"");
    }else{
        if(!own_side(current,r)||!is_free(r,c)||g.crater[r][c]||g.breach[r][c]){
            snprintf(notice,sizeof notice,"Invalid position. Place pieces on free, non-crater squares on your side.");return;
        }
        if(selection==1&&g.shields_placed[current]>=MAX_SHIELDS)return;
        if(selection==2&&g.cannons_this_round[current]>=MAX_CANNONS)return;
        save_undo();
        g.cells[r][c]=(Cell){selection==1?SHIELD:CANNON,current};
        if(selection==1){g.shields_placed[current]++;}
        else g.cannons_this_round[current]++;
        snprintf(notice,sizeof notice,"%s placed on %c%d. Continue placing or press E to end turn.",selection==1?"Shield":"Cannon",'A'+c,r+1);
    }
    /* Keep the selected placement tool active for rapid placement. */
    if(selection==4||selection==3)selection=0;
}

/* All shots are advanced using a common snapshot at each distance step. */
static void fire_phase(void){
    Shot shots[2*MAX_CANNONS];int count=0;
    for(int r=0;r<N;r++)for(int c=0;c<N;c++){
        Cell q=g.cells[r][c];
        if(q.piece==CANNON&&count<2*MAX_CANNONS){shots[count++]=(Shot){r,c,q.owner==0?1:-1,q.owner,1};}
    }
    int nuke_hit[2]={0,0};
    int nuke_flash[18],nflash=0;
    for(int p=0;p<2;p++)if(g.nuked[p]&&g.nuke_target[p].r>=0){
        Pos z=g.nuke_target[p];
        for(int r=z.r-1;r<=z.r+1;r++)for(int c=z.c-1;c<=z.c+1;c++)if(within(r,c)){
            if(g.cells[r][c].piece==KING)nuke_hit[g.cells[r][c].owner]=1;
            int cell=r*N+c,seen=0;
            for(int k=0;k<nflash;k++)if(nuke_flash[k]==cell)seen=1;
            if(!seen&&nflash<18)nuke_flash[nflash++]=cell;
        }
    }
    if(nflash){
        snprintf(notice,sizeof notice,"NUCLEAR DETONATION! 3x3 blast is instantly fatal to kings.");
        redraw();draw_grid(NULL,0,nuke_flash,nflash);delay_ms(700);
        for(int i=0;i<nflash;i++){
            int r=nuke_flash[i]/N,c=nuke_flash[i]%N;
            g.cells[r][c]=(Cell){EMPTY,-1};g.crater[r][c]=1;g.breach[r][c]=1;
        }
    }
    for(int i=0;i<count;i++)if(g.crater[shots[i].r][shots[i].c])shots[i].alive=0;
    int damage[2]={0,0};
    for(int step=1;step<=N;step++){
        int bullets[4],booms[4],nb=0,nboom=0,any=0;
        int hit_shield[N][N]={{0}};
        int award[N][N];memset(award,-1,sizeof award);
        for(int i=0;i<count;i++){
            Shot *s=&shots[i];if(!s->alive)continue;
            int r=s->r+s->dir*step,c=s->c;
            if(!within(r,c)){s->alive=0;continue;}
            any=1;
            Cell cell=g.cells[r][c];
            if(cell.piece==EMPTY||cell.piece==CANNON||(cell.piece==KING&&cell.owner==s->owner)){
                bullets[nb++]=r*N+c;continue;
            }
            s->alive=0;
            booms[nboom++]=r*N+c;
            if(cell.piece==SHIELD){hit_shield[r][c]=1;if(award[r][c]<0 && cell.owner!=s->owner)award[r][c]=s->owner;}
            if(cell.piece==KING&&cell.owner!=s->owner)damage[cell.owner]++;
        }
        if(!any)break;
        snprintf(notice,sizeof notice,"SIMULTANEOUS FIRE: volley %d. Cannons pass through other cannons.",step);
        redraw();draw_grid(bullets,nb,booms,nboom);delay_ms(250);
        for(int r=0;r<N;r++)for(int c=0;c<N;c++)if(hit_shield[r][c]){
            g.cells[r][c]=(Cell){EMPTY,-1};g.breach[r][c]=1;
            if(award[r][c]>=0)g.shields_hit[award[r][c]]++;
        }
    }
    for(int r=0;r<N;r++)for(int c=0;c<N;c++)if(g.cells[r][c].piece==CANNON)
        g.cells[r][c]=(Cell){EMPTY,-1};
    for(int p=0;p<2;p++){
        g.cannons_this_round[p]=0;
        g.lives[p]-=damage[p];
        if(g.lives[p]<0)g.lives[p]=0;
    }
    if(nuke_hit[0]||nuke_hit[1]||g.lives[0]==0||g.lives[1]==0){
        game_over=1;
        if((nuke_hit[0]||g.lives[0]==0)&&(nuke_hit[1]||g.lives[1]==0))victor=2;
        else victor=nuke_hit[0]||g.lives[0]==0?1:0;
        if(victor==2)snprintf(notice,sizeof notice,"BOTH KINGS WERE DESTROYED - DRAW!");
        else snprintf(notice,sizeof notice,"PLAYER %d WINS!",victor+1);
    }else{
        snprintf(notice,sizeof notice,"Round %d complete. Lives P1:%d P2:%d. Cannons reloaded.",round_no,g.lives[0],g.lives[1]);
    }
    for(int p=0;p<2;p++)g.nuke_target[p]=(Pos){-1,-1};
    redraw();delay_ms(800);
}

static void setup_game(void){
    memset(&g,0,sizeof g);
    for(int r=0;r<N;r++)for(int c=0;c<N;c++)g.cells[r][c].owner=-1;
    for(int p=0;p<2;p++){
        g.lives[p]=3;g.king[p]=(Pos){-1,-1};g.nuke_target[p]=(Pos){-1,-1};
        g.movement[p][1]=-1;
        g.movement[p][2]=5;g.movement[p][3]=5;g.movement[p][4]=5;g.movement[p][5]=3;
    }
}

typedef struct {int type,x,y;} Event; /* type: 1 mouse, 2 end turn, 3 quit */
static Event read_event(int timeout_ms){
    static char seq[64];static int state=0,len=0;
    double until=mono()+timeout_ms/1000.0;
    for(;;){
        double rem=until-mono();if(rem<=0)return (Event){0,0,0};
        fd_set s;FD_ZERO(&s);FD_SET(STDIN_FILENO,&s);
        struct timeval tv={(long)rem,(long)((rem-(long)rem)*1e6)};
        int rc=select(STDIN_FILENO+1,&s,NULL,NULL,&tv);
        if(rc<=0){if(rc<0&&errno==EINTR)continue;return (Event){0,0,0};}
        unsigned char ch;if(read(STDIN_FILENO,&ch,1)!=1)continue;
        if(state==0){
            if(ch==27){state=1;continue;}
            if(ch=='e'||ch=='E'||ch=='\r'||ch=='\n')return (Event){2,0,0};
            if(ch=='q'||ch=='Q')return (Event){3,0,0};
            continue;
        }
        if(state==1){state=ch=='['?2:0;continue;}
        if(state==2){state=ch=='<'?3:0;len=0;continue;}
        if(state==3){
            if((ch>='0'&&ch<='9')||ch==';'){
                if(len<(int)sizeof seq-1)seq[len++]=(char)ch;
                else state=0;
                continue;
            }
            seq[len]='\0';state=0;
            int b,x,y;
            if(ch=='M'&&sscanf(seq,"%d;%d;%d",&b,&x,&y)==3&&(b&3)==0&&b<64)
                return (Event){1,x,y};
        }
    }
}
static int board_cell_from_xy(int x,int y,int *r,int *c){
    if(x<BX+1||x>=XEND+1||y<BY+1||y>BY+4*N-1)return 0;
    int dx=x-(BX+1),dy=y-(BY+1);
    if(dx%BW>=7||dy%BH>=3)return 0;
    *r=dy/BH;*c=dx/BW;
    return within(*r,*c);
}
static void handle_click(int x,int y,int in_placement){
    int r,c;
    if(board_cell_from_xy(x,y,&r,&c)){
        if(in_placement){if(own_side(current,r)&&is_free(r,c)){
            g.cells[r][c]=(Cell){KING,current};g.king[current]=(Pos){r,c};
            snprintf(notice,sizeof notice,"Player %d placed king at %c%d.",current+1,'A'+c,r+1);
        }else snprintf(notice,sizeof notice,"Choose a square in your territory.");}
        else select_square(r,c);
        return;
    }
    if(in_placement)return;
    if(y>=5&&y<=8){
        if(x>=1&&x<=17)select_tool(1);
        else if(x>=18&&x<=34)select_tool(2);
        else if(x>=35&&x<=51)select_tool(3);
        else if(x>=52&&x<=64)select_tool(4);
        else if(x>=65&&x<=75){
            undo();
        }else if(x>=76&&x<=85){selection=0;snprintf(notice,sizeof notice,"Selection canceled.");}
    }else if(y>=57&&y<=58){
        int type=(x-1)/16+1;
        if(x>=1&&type>=1&&type<=5){
            if(g.lives[current]>0&&available_moves(current,type)>0){selection=3;move_type=type;
                snprintf(notice,sizeof notice,"%s selected: yellow squares are legal; red ! squares cost lives.",
                    (const char*[]){"","STEP","SKEW","LEAP","AXIS","JASMIN"}[type]);}
            else snprintf(notice,sizeof notice,"No uses left for this movement.");
        }
    }
}
static int king_placement(int p){
    current=p;selection=0;history_len=0;deadline=mono()+TURN_SECONDS;
    snprintf(notice,sizeof notice,"PLAYER %d: click an empty square in your %s territory to place your king.",p+1,p==0?"RED":"GREEN");
    redraw();
    while(mono()<deadline){
        Event e=read_event(150);
        if(e.type==3)return -1;
        if(e.type==1)handle_click(e.x,e.y,1);
        if(g.king[p].r>=0){redraw();return 1;}
        tick();
        if(e.type==1)redraw();
    }
    /* Always place a king: on timeout use the center of own territory. */
    int r=p==0?2:7,c=4;
    g.cells[r][c]=(Cell){KING,p};g.king[p]=(Pos){r,c};
    snprintf(notice,sizeof notice,"Player %d's king placed automatically after timeout.",p+1);
    return 1;
}
static int player_turn(int p){
    current=p;selection=0;move_type=1;history_len=0;
    deadline=mono()+TURN_SECONDS;
    snprintf(notice,sizeof notice,"PLAYER %d: click action boxes, then squares. Press E or ENTER to end turn.",p+1);
    redraw();
    while(!game_over&&mono()<deadline){
        Event e=read_event(160);
        if(e.type==3)return -1;
        if(e.type==2){snprintf(notice,sizeof notice,"Player %d ended the turn.",p+1);break;}
        if(e.type==1){handle_click(e.x,e.y,0);redraw();}
        tick();
    }
    if(!game_over&&mono()>=deadline)snprintf(notice,sizeof notice,"Player %d's timer expired.",p+1);
    selection=0;history_len=0;
    return 1;
}
int main(void){
    struct winsize ws;
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)){
        fprintf(stderr,"Run in Linux Mint's actual terminal, not redirected input.\n");return 1;
    }
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)!=0||ws.ws_col<87||ws.ws_row<62){
        fprintf(stderr,"Maximize your terminal or decrease font size: need at least 87 columns x 62 rows.\n");return 1;
    }
    setup_game();
    atexit(restore_terminal);signal(SIGINT,interrupted);signal(SIGTERM,interrupted);
    if(!setup_terminal()){fprintf(stderr,"Failed to enable terminal mouse input.\n");return 1;}
    for(int p=0;p<2;p++)if(king_placement(p)<0)return 0;
    while(!game_over){
        for(int p=0;p<2;p++){
            if(player_turn(p)<0)return 0;
            if(game_over)break;
        }
        if(game_over)break;
        deadline=0;
        fire_phase();
        if(!game_over)round_no++;
    }
    deadline=0;
    if(victor==2)snprintf(notice,sizeof notice,"DRAW: Both kings were defeated. Press Q to quit.");
    else snprintf(notice,sizeof notice,"PLAYER %d WINS! Press Q to quit.",victor+1);
    redraw();
    while(read_event(500).type!=3){}
    return 0;
}
