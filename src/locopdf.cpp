/***************************************************************************
 *   Copyright (C) 2008 by Marc Lajoie                                     *
 *   quickhand@openinkpot.org                                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/



#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <cstdlib>
#include <cmath>
#include <Evas.h>
#include <Ecore.h>
#include <Ecore_File.h>
#include <Ecore_Evas.h>
#include <Edje.h>
#include <epdf/Epdf.h>
#include <GlobalParams.h>
#include "keyhandler.h"
#include "dialogs.h"
#include "locopdf.h"
#include "database.h"
#define REL_THEME "themes/themes_oitheme.edj"

#define ROUND(f) (int)floor(f + 0.5)

using namespace std;


pthread_t thread;

pthread_mutex_t pdf_renderer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t pdf_page_event = PTHREAD_COND_INITIALIZER;

Evas *evas;
Epdf_Document *document;
Epdf_Page     *page;
Ecore_List *pdf_index;
char          *filename;

Evas_Object *active_image;
static const int INVALID_PAGE = -1;

int numpages;
int curpage=0;
int fitmode=FIT_WIDTH;
int readermode=0;
double zoom=1.0;
double zoominc=0.1;
double hpaninc=0.5;
double vpaninc=0.5;

int lefttrim=0;
int righttrim=0;
int toptrim=0;
int bottomtrim=0;


int winwidth=600;
int winheight=800;
/*
 * Returns edje theme file name.
 */
 
char *get_theme_file()
{
 	//char *cwd = get_current_dir_name();
	char *rel_theme;
	asprintf(&rel_theme, "%s/%s", "/usr/share/locopdf", REL_THEME);
    //asprintf(&rel_theme, "%s/%s",cwd, REL_THEME);
	//free(cwd);
	return rel_theme;
}

int get_win_width()
{
    return winwidth;    
}
int get_win_height()
{
    return winheight;    
}
double get_zoom_inc()
{
    return zoominc;    
}
void set_zoom_inc(double newzoominc)
{
    zoominc=newzoominc;    
}
double get_hpan_inc()
{
    return hpaninc;    
}
void set_hpan_inc(double newhpaninc)
{
    hpaninc=newhpaninc;    
}
double get_vpan_inc()
{
    return vpaninc;    
}
void set_vpan_inc(double newvpaninc)
{
    vpaninc=newvpaninc;    
}
int get_lefttrim()
{
    return lefttrim;    
}
void set_lefttrim(int newlefttrim)
{
    lefttrim=newlefttrim;    
}
int get_righttrim()
{
    return righttrim;    
}
void set_righttrim(int newrighttrim)
{
    righttrim=newrighttrim;    
}
int get_toptrim()
{
    return toptrim;    
}
void set_toptrim(int newtoptrim)
{
    toptrim=newtoptrim;    
}
int get_bottomtrim()
{
    return bottomtrim;    
}
void set_bottomtrim(int newbottomtrim)
{
    bottomtrim=newbottomtrim;    
}
int get_fit_mode()
{
    return fitmode;    
}
void set_fit_mode(int newfitmode)
{
    fitmode=newfitmode;
}
int get_reader_mode()
{
    return readermode;    
}
void set_reader_mode(int newreadermode)
{
    readermode=(newreadermode!=0);    
    
}
int get_antialias_mode()
{
    return (globalParams->getAntialias() && globalParams->getVectorAntialias());
    
}
void set_antialias_mode(int newantialiasmode)
{
    if(newantialiasmode)
    {
        globalParams->setAntialias("yes");
        globalParams->setVectorAntialias("yes");
    }
    else
    {
        globalParams->setAntialias("no");
        globalParams->setVectorAntialias("no");
        
    }
    
}
int get_num_pages()
{
    return numpages;
}
void goto_page(int newpage)
{
    curpage=newpage;
    show_cur_page();
    reset_cur_panning();
}
int get_cur_page()
{
    return curpage;    
}
Epdf_Document *get_document()
{
    return document;    
}

static const int cache_size = 3;
static struct cache_item {
	int page;
	int displayed;
	Evas_Object *obj;
} cache[cache_size];

/* prediction for the next page. */
static int prediction = INVALID_PAGE;
static int cache_invalidated = 0;

static void invalidate_cache()
{
    pthread_mutex_lock(&pdf_renderer_mutex);
    cache_invalidated = 1;
    pthread_mutex_unlock(&pdf_renderer_mutex);
}

static cache_item *find_unused_obj()
{
    struct cache_item *item = cache;
    for(int i = 0; i < cache_size; i++, item++) {
        if(!item->displayed) {
            fprintf(stderr, "using slot %d\n", i);
            return item;
        }
    }
    return NULL;
}

static void render_page(int page, Evas_Object *obj);
static void image_cache_clear_synch();

static void *thread_func(void *vptr_args)
{
    pthread_mutex_lock(&pdf_renderer_mutex);

    while(1) {
        while(prediction == INVALID_PAGE && !cache_invalidated)
            pthread_cond_wait(&pdf_page_event, &pdf_renderer_mutex);

        if(cache_invalidated) {
            image_cache_clear_synch();
            cache_invalidated = 0;
            continue;
        }

        // Right now prediction is valid and the page_event has fired.
        // Looks like we have to render the page
        fprintf(stderr, "gonna render %d\n", prediction);

        cache_item *item = find_unused_obj();
        assert(item != NULL);

        int page = prediction;
        item->page = INVALID_PAGE;
        pthread_mutex_unlock(&pdf_renderer_mutex);

        // Rendering takes place out of the mutex.
        render_page(page, item->obj);

        pthread_mutex_lock(&pdf_renderer_mutex);
        if(!cache_invalidated) { // maybe even this page is dirty.
            item->page = page;
            if(prediction == page)
                prediction = INVALID_PAGE;
            pthread_cond_signal(&pdf_page_event);
        }
    }

    pthread_mutex_unlock(&pdf_renderer_mutex);
}

static void undisplay_image(Evas_Object *image)
{
    pthread_mutex_lock(&pdf_renderer_mutex);
    for(int i = 0; i < cache_size; i++) {
        if(cache[i].obj == image) {
            fprintf(stderr, "undisplaying image at slot %d\n", i);
            cache[i].displayed = 0;
        }
    }
    pthread_mutex_unlock(&pdf_renderer_mutex);
}

static cache_item *try_find_slot_synch(int page)
{
    if(cache_invalidated)
        return NULL;

    for(int i = 0; i < cache_size; i++) {
        if(cache[i].page == page)
            return &cache[i];
    }
    return NULL;
}

static void request_page_synch(int page);

static Evas_Object *find_image_for_page(int page)
{
    cache_item *ret;
    pthread_mutex_lock(&pdf_renderer_mutex);

    while((ret = try_find_slot_synch(page)) == NULL) {
        request_page_synch(page);
        pthread_cond_wait(&pdf_page_event, &pdf_renderer_mutex);
    }
    ret->displayed = 1;
    
    pthread_mutex_unlock(&pdf_renderer_mutex);
    return ret->obj;
}

static void render_page(int curpage, Evas_Object *pdfobj)
{
    epdf_page_page_set(page,curpage);
    int width,height;
    epdf_page_size_get (page, &width, &height);

    double fitwidthzoom=((double)get_win_width())/((double)(width-lefttrim-righttrim))*zoom;
    double fitheightzoom=((double)get_win_height())/((double)(height-toptrim-bottomtrim))*zoom;
    
    double scalex;
    double scaley;
    
    if(fitmode==FIT_WIDTH)
    {
        scalex=fitwidthzoom;    
        scaley=fitwidthzoom;
    }
    else if(fitmode==FIT_HEIGHT)
    {
        scalex=fitheightzoom;
        scaley=fitheightzoom;
    }
    else if(fitmode==FIT_BEST)
    {
        if(fitwidthzoom<=fitheightzoom)
        {
            scalex=fitwidthzoom;
            scaley=fitwidthzoom;
        }
        else
        {
            scalex=fitheightzoom;
            scaley=fitheightzoom;
        }
    }
    else if(fitmode==FIT_STRETCH)
    {
        scalex=fitwidthzoom;
        scaley=fitheightzoom;
    }
    else if(fitmode==FIT_NO)
    {
        scalex=1.0;
        scaley=1.0;
    }
    
    epdf_page_scale_set (page,scalex,scaley);
    if(!lefttrim && !righttrim && !toptrim && !bottomtrim)
    {
        epdf_page_render (page,pdfobj);
    }
    else
    {
        epdf_page_render_slice (page, pdfobj,
                (int)(((double)lefttrim)*scalex),
                (int)(((double)toptrim)*scaley),
                (int)(((double)(width-lefttrim-righttrim))*scalex),
                (int)(((double)(height-toptrim-bottomtrim))*scaley));
    }
}

static void image_cache_init()
{
    for(int i = 0; i < cache_size; i++) {
        cache_item *item = cache + i;
        item->page = INVALID_PAGE;
        item->displayed = 0;
        item->obj = evas_object_image_add(evas);
        evas_object_move(item->obj, 0, 0);
    }
}

static void image_cache_clear_synch()
{
    for(int i = 0; i < cache_size; i++) {
        cache[i].page = INVALID_PAGE;
    }
}

int are_legal_coords(int x1,int y1,int x2,int y2)
{
    
    int xs_in_range=((x1>0&&x1<get_win_width())||(x2>0&&x2<get_win_width()));
    int ys_in_range=((y1>0&&y1<get_win_height())||(y2>0&&y2<get_win_height()));
    int xs_opposite=(x1<=0&&x2>=get_win_width());
    int ys_opposite=(y1<=0&&y2>=get_win_height());
    if((ys_in_range && xs_in_range) || (ys_in_range&& xs_opposite) || (xs_in_range && ys_opposite) || (xs_opposite && ys_opposite))
        return 1;
    return 0;
}

static void pan_cur_page(int panx,int pany)
{
    int x,y,w,h;
    evas_object_geometry_get(active_image,&x,&y,&w,&h);
    
    if(are_legal_coords(x+panx,y+pany,x+w+panx,y+h+pany))
        evas_object_move (active_image,x+panx,y+pany);
}

void reset_cur_panning()
{
    evas_object_move (active_image, 0, 0);    
}

static void predict_page(int page)
{
    pthread_mutex_lock(&pdf_renderer_mutex);
    request_page_synch(page);
    pthread_mutex_unlock(&pdf_renderer_mutex);
}

static void request_page_synch(int page)
{
    if(prediction != page && !try_find_slot_synch(page)) {
        prediction = page;
        pthread_cond_signal(&pdf_page_event);
        fprintf(stderr, "requested page %d\n", page);
    }
}


void show_cur_page()
{
    fprintf(stderr, "trying to show page %d\n", curpage);
    Evas_Object *newimage = find_image_for_page(curpage);

    if(newimage == active_image)
        return;

    // FIXME: where do we check that curpage+1 exists?
    predict_page(curpage + 1);

    if(active_image) {
        evas_object_hide(active_image);
        undisplay_image(active_image);
    }
    evas_object_show(newimage);
    active_image = newimage;
}

static void next_page()
{
    if(curpage>=(numpages-1))
        return;
    curpage++;
    show_cur_page();
    reset_cur_panning();
}

static void prev_page()
{
    if(curpage<=0)
        return;
    curpage--;
    show_cur_page();
    reset_cur_panning();
}

/* GUI */

/* Main key handler */

void main_esc(Evas *e, Evas_Object *obj)
{
    ecore_main_loop_quit();
}

void main_ok(Evas *e, Evas_Object *obj)
{
    Evas_Object *bgobj=evas_object_name_find(evas,"background");
    PreferencesDialog(evas,bgobj);
    
}

void main_plus(Evas *e, Evas_Object *obj)
{
    Evas_Object *pdfobj = active_image;
    int x,y,w,h;
    evas_object_geometry_get(pdfobj,&x,&y,&w,&h);
    int new_w=ROUND(((double)w)*(zoom+zoominc)/zoom);
    int new_h=ROUND(((double)h)*(zoom+zoominc)/zoom);
    if(are_legal_coords(x,y,x+new_w,y+new_h))
    {
        zoom+=zoominc;
        show_cur_page();
    }
}
void main_minus(Evas *e, Evas_Object *obj)
{
    if((zoom-zoominc)>0)
    {
        Evas_Object *pdfobj = active_image;
        int x,y,w,h;
        evas_object_geometry_get(pdfobj,&x,&y,&w,&h);
        int new_w=ROUND(((double)w)*(zoom-zoominc)/zoom);
        int new_h=ROUND(((double)h)*(zoom-zoominc)/zoom);
        if(are_legal_coords(x,y,x+new_w,y+new_h))
        {
            zoom-=zoominc;
            show_cur_page();
        }
    }
}
void main_nav_up(Evas *e, Evas_Object *obj)
{
    
}

void main_nav_down(Evas *e, Evas_Object *obj)
{
    
}

void main_nav_left(Evas *e, Evas_Object *obj)
{
    
    prev_page();
}

void main_nav_right(Evas *e, Evas_Object *obj)
{
    if(readermode)
    {
        Evas_Object *pdfobj = active_image;
        int pan_amt=(-1)*ROUND(((double)get_win_height())*vpaninc);
        int x,y,w,h;
        evas_object_geometry_get(pdfobj,&x,&y,&w,&h);
    
    
        if(are_legal_coords(x,y+pan_amt,x+w,y+h+pan_amt))
            pan_cur_page(0,pan_amt);
        else
            next_page();
    }
    else
        next_page();
}

void main_nav_sel(Evas *e, Evas_Object *obj)
{
    
    
}
void main_nav_menubtn(Evas *e, Evas_Object *obj)
{
    
    
    
}

void main_item(Evas *e, Evas_Object *obj,int index, bool lp)
{
    if(index==1)
    {
        Evas_Object *bgobj=evas_object_name_find(evas,"background");
        GotoPageEntry(evas,bgobj);  
    }
    else if(index==2)
    {
        pan_cur_page(0,ROUND(((double)get_win_height())*vpaninc));
    }
    else if(index==3)
    {
        if(pdf_index)
            TOCDialog(e,obj,pdf_index);
    }
    else if(index==4)
    {
        
    }
    else if(index==5)
    {
        //reset_cur_panning();
    }
    else if(index==6)
    {
        pan_cur_page(ROUND(((double)get_win_width())*hpaninc),0);
    }
    else if(index==7)
    {
        pan_cur_page(0,(-1)*ROUND(((double)get_win_height())*vpaninc));
    }
    else if(index==8)
    {
        pan_cur_page((-1)*ROUND(((double)get_win_width())*hpaninc),0);
    }
    else if(index==9)
    {
        prev_page();    
    }
    else if(index==0)
    {
        if(readermode)
        {
            Evas_Object *pdfobj = active_image;
            int pan_amt=(-1)*ROUND(((double)get_win_height())*vpaninc);
            int x,y,w,h;
            evas_object_geometry_get(pdfobj,&x,&y,&w,&h);
    
    
            if(are_legal_coords(x,y+pan_amt,x+w,y+h+pan_amt))
                pan_cur_page(0,pan_amt);
            else
                next_page();
        }
        else
            next_page();
    }
}

static key_handler_info_t main_info =
{
    main_ok,
    main_esc,
    main_nav_up,
    main_nav_down,
    main_nav_left,
    main_nav_right,
    main_nav_sel,
    main_nav_menubtn,
    main_plus,
    main_minus,
    main_item
};

void save_global_settings(char *filename)
{
    set_setting_INT(filename,"current_page",curpage);
    set_setting_DOUBLE(filename,"zoom_increment",zoominc);
    set_setting_DOUBLE(filename,"current_zoom",zoom);
    set_setting_DOUBLE(filename,"h_pan_increment",hpaninc);
    set_setting_DOUBLE(filename,"v_pan_increment",vpaninc);
    set_setting_INT(filename,"left_trim",lefttrim);
    set_setting_INT(filename,"right_trim",righttrim);
    set_setting_INT(filename,"top_trim",toptrim);
    set_setting_INT(filename,"bottom_trim",bottomtrim);
    set_setting_INT(filename,"fit_mode",fitmode);
    set_setting_INT(filename,"reader_mode",readermode);
}
void restore_global_settings(char *filename)
{
    int temp11,temp12,temp13,temp14;
    double temp21,temp22,temp23,temp24;
    temp11=get_setting_INT(filename,"current_page");
    if(temp11>=0)
        curpage=temp11;
    
    
    temp21=get_setting_DOUBLE(filename,"zoom_increment");
    temp22=get_setting_DOUBLE(filename,"current_zoom");
    if(temp21>0 && temp22>0)
    {
        zoominc=temp21;
        zoom=temp22;
    }
    temp21=get_setting_DOUBLE(filename,"h_pan_increment");
    temp22=get_setting_DOUBLE(filename,"v_pan_increment");
    if(temp21>0 && temp22>0)
    {
        hpaninc=temp21;
        vpaninc=temp22;
        
    }
    temp11=get_setting_INT(filename,"left_trim");
    temp12=get_setting_INT(filename,"right_trim");
    temp13=get_setting_INT(filename,"top_trim");
    temp14=get_setting_INT(filename,"bottom_trim");
    if(temp11>=0 && temp12>=0 && temp13>=0 && temp14>=0)
    {
        lefttrim=temp11;
        righttrim=temp12;
        toptrim=temp13;
        bottomtrim=temp14;
        
    }
    temp11=get_setting_INT(filename,"reader_mode");
    if(temp11==0 || temp11==1)
    {
        readermode=temp11;    
    }
    temp11=get_setting_INT(filename,"fit_mode");
    if(temp11>=0)
    {
        fitmode=temp11;    
    }
}

int main(int argc, char *argv[])
{
    Ecore_Evas *ee;
    
    Evas_Object *bg,*o1,*o2;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.pdf\n", argv[0]);
        exit(1);
    }

    /* initialize our libraries */
    evas_init();
    ecore_init();
    ecore_evas_init();
    edje_init();
    
    if (!globalParams)
        globalParams = new GlobalParams();
    globalParams->setAntialias("yes");
    globalParams->setVectorAntialias("yes");
    /* setup database */
    
    const char *homedir=getenv("HOME");
    char *settingsdir;
    asprintf(&settingsdir,"%s/%s",homedir,".locopdf/");
    if(!ecore_file_path_dir_exists(settingsdir))
    {
        ecore_file_mkpath(settingsdir);
    }
    free(settingsdir);
    char *dbfile;
    asprintf(&dbfile,"%s/%s",homedir,".locopdf/files.db");
    int dbres=init_database(dbfile);
    free(dbfile);
    if(dbres!=(-1))
        restore_global_settings(argv[1]);
    /* create our Ecore_Evas and show it */
    ee = ecore_evas_software_x11_new(0, 0, 0, 0, 600, 800);
    assert(ee != NULL);
    
    ecore_evas_borderless_set(ee, 0);
    ecore_evas_shaped_set(ee, 0);
    ecore_evas_title_set(ee, "LoCoPDF");
    ecore_evas_show(ee);

    /* get a pointer our new Evas canvas */
    evas = ecore_evas_get(ee);

    /* create our white background */
    bg = evas_object_rectangle_add(evas);
    evas_object_color_set(bg, 255, 255, 255, 255);
    evas_object_move(bg, 0, 0);
    evas_object_resize(bg, 600, 800);
    evas_object_name_set(bg, "background");
    evas_object_focus_set(bg, 1);
    set_key_handler(bg,&main_info);
    evas_object_show(bg);
    
    
    filename=argv[1];
    document = epdf_document_new (argv[1]);
    if (!document) {
    // manage error here
        fprintf(stderr,"Error Opening Document");
    }

    numpages=epdf_document_page_count_get(document);
    page = epdf_page_new (document);
    if (!page) {
    // manage error here
        fprintf(stderr,"Error Processing Document");
    }

    active_image = NULL;
    image_cache_init();
    
    pdf_index=epdf_index_new (document);
    
    char *temp11 = NULL, *temp12 = NULL;
    int current_x, current_y;
    if(dbres!=(-1))
    {
        temp11=get_setting(argv[1],"current_x");
        temp12=get_setting(argv[1],"current_y");
    }
    if(temp11 && temp12) {
        current_x = strtol(temp11, NULL, 10);
        current_y = strtol(temp12, NULL, 10);
    }
    if(temp11)
        free(temp11);
    if(temp12)
        free(temp12);

    if(dbres!=(-1))
    {
        int am=get_setting_INT(argv[1],"antialias");
        if(am>=0)
            set_antialias_mode(am);
    }
    
    pthread_create(&thread, NULL, thread_func, NULL);
    show_cur_page();

    /* start the main event loop */
    ecore_main_loop_begin();
    
    /* when the main event loop exits, shutdown our libraries */
    if(dbres!=(-1))
    {
        save_global_settings(argv[1]);
        Evas_Object *pdfobj = active_image;
        int x,y,w,h;
        evas_object_geometry_get(pdfobj,&x,&y,&w,&h);
        set_setting_INT(argv[1],"current_x",x);
        set_setting_INT(argv[1],"current_y",y);
        set_setting_INT(argv[1],"antialias",get_antialias_mode());
        fini_database();
    }
    evas_object_del (bg);
    epdf_index_delete(pdf_index);
    epdf_page_delete (page);
    epdf_document_delete (document);

    edje_shutdown();
    ecore_evas_shutdown();
    ecore_shutdown();
    evas_shutdown();
}
// vim:set et sw=4:
