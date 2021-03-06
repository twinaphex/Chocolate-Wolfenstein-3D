// WL_DRAW.C

#include "wl_def.h"
#include "retro_endian.h"

/*
=============================================================================

                               LOCAL CONSTANTS

=============================================================================
*/

/* the door is the last picture before the sprites */
#define DOORWALL        (PMSpriteStart-8)

#define ACTORSIZE       0x4000

/*
=============================================================================

                              GLOBAL VARIABLES

=============================================================================
*/

static byte *vbuf = NULL;
unsigned vbufPitch = 0;

int32_t    lasttimecount;
int32_t    frameon;
boolean fpscounter;

int fps_frames=0, fps_time=0, fps=0;

int *wallheight;
int min_wallheight;

/* math tables */
short *pixelangle;
int32_t finetangent[FINEANGLES/4];
fixed sintable[ANGLES+ANGLES/4];
fixed *costable = sintable+(ANGLES/4);

/* refresh variables */
fixed   viewx,viewy; /* the focal point */
short   viewangle;
fixed   viewsin,viewcos;

/* wall optimization variables */
int     lastside;               /* true for vertical */
int32_t    lastintercept;
int     lasttilehit;
int     lasttexture;

/* ray tracing variables */
short    focaltx,focalty,viewtx,viewty;
longword xpartialup,xpartialdown,ypartialup,ypartialdown;

short   midangle,angle;

word    tilehit;
int     pixx;

short   xtile,ytile;
short   xtilestep,ytilestep;
int32_t    xintercept,yintercept;
word    xstep,ystep;
word    xspot,yspot;
int     texdelta;

word horizwall[MAXWALLTILES],vertwall[MAXWALLTILES];


/*
============================================================================

                           3 - D  DEFINITIONS

============================================================================
*/

/*
========================
=
= TransformActor
=
= Takes paramaters:
=   gx,gy               : globalx/globaly of point
=
= globals:
=   viewx,viewy         : point of view
=   viewcos,viewsin     : sin/cos of viewangle
=   scale               : conversion from global value to screen value
=
= sets:
=   screenx,transx,transy,screenheight: projected edge location and size
=
========================
*/

static void TransformActor (objtype *ob)
{
   fixed ny;

   /* translate point to view centered coordinates */
   fixed gx = ob->x-viewx;
   fixed gy = ob->y-viewy;

   /* calculate newx */
   fixed gxt = FixedMul(gx,viewcos);
   fixed gyt = FixedMul(gy,viewsin);

   /* fudge the shape forward a bit, because
    * the midpoint could put parts of the shape
    * into an adjacent wall. */
   fixed nx = gxt-gyt-ACTORSIZE;

   /* calculate newy */
   gxt = FixedMul(gx,viewsin);
   gyt = FixedMul(gy,viewcos);
   ny = gyt+gxt;

   /* calculate perspective ratio */
   ob->transx = nx;
   ob->transy = ny;

   /* too close, don't overflow the divide */
   if (nx < MINDIST)                 
   {
      ob->viewheight = 0;
      return;
   }

   ob->viewx = (word)(centerx + ny*scale/nx);

   /* calculate height (heightnumerator/(nx>>8)) */
   ob->viewheight = (word)(heightnumerator/(nx>>8));
}

/*
========================
=
= TransformTile
=
= Takes paramaters:
=   tx,ty               : tile the object is centered in
=
= globals:
=   viewx,viewy         : point of view
=   viewcos,viewsin     : sin/cos of viewangle
=   scale               : conversion from global value to screen value
=
= sets:
=   screenx,transx,transy,screenheight: projected edge location and size
=
= Returns true if the tile is withing getting distance
=
========================
*/

static boolean TransformTile (int tx, int ty, short *dispx, short *dispheight)
{
   fixed ny;

   /* translate point to view centered coordinates */
   fixed gx  = ((int32_t)tx<<TILESHIFT)+0x8000-viewx;
   fixed gy  = ((int32_t)ty<<TILESHIFT)+0x8000-viewy;

   /* calculate newx */
   fixed gxt = FixedMul(gx,viewcos);
   fixed gyt = FixedMul(gy,viewsin);
   fixed nx  = gxt-gyt-0x2000;            /* 0x2000 is size of object */

   /* calculate newy */
   gxt       = FixedMul(gx,viewsin);
   gyt       = FixedMul(gy,viewcos);
   ny        = gyt+gxt;

   /* calculate height / perspective ratio */
   
   /* too close, don't overflow the divide */
   if (nx < MINDIST)                 
      *dispheight = 0;
   else
   {
      *dispx      = (short)(centerx + ny*scale/nx);
      *dispheight = (short)(heightnumerator/(nx>>8));
   }

   /* see if it should be grabbed */
   if (nx < TILEGLOBAL && ny > -TILEGLOBAL/2 && ny < TILEGLOBAL/2)
      return true;
   return false;
}

/*
====================
=
= CalcHeight
=
= Calculates the height of xintercept,yintercept from viewx,viewy
=
====================
*/

static int CalcHeight(void)
{
   int height;
   fixed z = FixedMul(xintercept - viewx, viewcos) - FixedMul(yintercept - viewy, viewsin);

   if (z < MINDIST)
      z = MINDIST;

   height = heightnumerator / (z >> 8);

   if(height < min_wallheight)
      min_wallheight = height;

   return height;
}

/*
===================
=
= ScalePost
=
===================
*/

static byte *postsource;
static int postx;
static int postwidth;

static void ScalePost(void)
{
   int yoffs, yw, yd, yendoffs;
   byte col;
   int ywcount = yd = wallheight[postx] >> 3;

   if(yd <= 0)
      yd      = 100;

   yoffs      = (viewheight / 2 - ywcount) * vbufPitch;

   if (yoffs < 0)
      yoffs   = 0;

   yoffs     += postx;

   yendoffs   = viewheight / 2 + ywcount - 1;
   yw         = TEXTURESIZE-1;

   while(yendoffs >= viewheight)
   {
      ywcount -= TEXTURESIZE/2;

      while(ywcount <= 0)
      {
         ywcount += yd;
         yw--;
      }
      yendoffs--;
   }

   if(yw < 0)
      return;

   col      = postsource[yw];
   yendoffs = yendoffs * vbufPitch + postx;

   while(yoffs <= yendoffs)
   {
      vbuf[yendoffs]  = col;
      ywcount        -= TEXTURESIZE/2;

      if (ywcount <= 0)
      {
         do
         {
            ywcount += yd;
            yw--;
         }while(ywcount <= 0);

         if(yw < 0)
            break;
         col = postsource[yw];
      }
      yendoffs -= vbufPitch;
   }
}

/*
====================
=
= HitVertWall
=
= tilehit bit 7 is 0, because it's not a door tile
= if bit 6 is 1 and the adjacent tile is a door tile, use door side pic
=
====================
*/

static void HitVertWall (void)
{
   int wallpic;
   int texture = ((yintercept+texdelta)>>TEXTUREFROMFIXEDSHIFT)&TEXTUREMASK;

   if (xtilestep == -1)
   {
      texture = TEXTUREMASK-texture;
      xintercept += TILEGLOBAL;
   }

   if (lastside == 1 && lastintercept==xtile && lasttilehit==tilehit && !(lasttilehit & 0x40))
   {
      ScalePost();

      if((pixx&3) && texture == lasttexture)
      {
         postx = pixx;
         wallheight[pixx] = wallheight[pixx-1];
         return;
      }

      wallheight[pixx]    = CalcHeight();
      postsource         += texture-lasttexture;
      postwidth           = 1;
      postx               = pixx;
      lasttexture         = texture;
      return;
   }

   if (lastside != -1)
      ScalePost();

   lastside         = 1;
   lastintercept    = xtile;
   lasttilehit      = tilehit;
   lasttexture      = texture;
   wallheight[pixx] = CalcHeight();
   postx            = pixx;
   postwidth        = 1;

   /* check for adjacent doors */
   if (tilehit & 0x40)
   {                                                               
      ytile = (short)(yintercept>>TILESHIFT);

      if ( tilemap[xtile-xtilestep][ytile]&0x80 )
         wallpic = DOORWALL+3;
      else
         wallpic = vertwall[tilehit & ~0x40];
   }
   else
      wallpic = vertwall[tilehit];

   postsource = PM_GetTexture(wallpic) + texture;
}


/*
====================
=
= HitHorizWall
=
= tilehit bit 7 is 0, because it's not a door tile
= if bit 6 is 1 and the adjacent tile is a door tile, use door side pic
=
====================
*/

static void HitHorizWall(void)
{
   int wallpic;
   int texture = ((xintercept+texdelta)>>TEXTUREFROMFIXEDSHIFT)&TEXTUREMASK;

   if (ytilestep == -1)
      yintercept += TILEGLOBAL;
   else
      texture = TEXTUREMASK-texture;

   if (lastside == 0 
         && lastintercept == ytile
         && lasttilehit   == tilehit
         && !(lasttilehit & 0x40))
   {
      ScalePost();
      if((pixx&3) && texture == lasttexture)
      {
         postx=pixx;
         wallheight[pixx] = wallheight[pixx-1];
         return;
      }
      wallheight[pixx]    = CalcHeight();
      postsource         += texture-lasttexture;
      postwidth           = 1;
      postx               = pixx;
      lasttexture         = texture;
      return;
   }

   if (lastside != -1)
      ScalePost();

   lastside               = 0;
   lastintercept          = ytile;
   lasttilehit            = tilehit;
   lasttexture            = texture;
   wallheight[pixx]       = CalcHeight();
   postx                  = pixx;
   postwidth              = 1;

   /* check for adjacent doors */
   if (tilehit & 0x40)
   {
      xtile = (short)(xintercept>>TILESHIFT);
      if ( tilemap[xtile][ytile-ytilestep]&0x80)
         wallpic = DOORWALL+2;
      else
         wallpic = horizwall[tilehit & ~0x40];
   }
   else
      wallpic = horizwall[tilehit];

   postsource = PM_GetTexture(wallpic) + texture;
}

/*
====================
=
= HitHorizDoor
=
====================
*/

static void HitHorizDoor (void)
{
   int doorpage;
   int doornum = tilehit&0x7f;
   int texture = ((xintercept-doorposition[doornum])>>TEXTUREFROMFIXEDSHIFT)&TEXTUREMASK;

   if(lasttilehit==tilehit)
   {
      ScalePost();
      if((pixx&3) && texture == lasttexture)
      {
         postx            = pixx;
         wallheight[pixx] = wallheight[pixx-1];
         return;
      }
      wallheight[pixx]  = CalcHeight();
      postsource       += texture-lasttexture;
      postwidth         = 1;
      postx             = pixx;
      lasttexture       = texture;
      return;
   }

   if (lastside != -1)
      ScalePost();

   lastside         = 2;
   lasttilehit      = tilehit;
   lasttexture      = texture;
   wallheight[pixx] = CalcHeight();
   postx            = pixx;
   postwidth        = 1;

   switch(doorobjlist[doornum].lock)
   {
      case dr_normal:
         doorpage = DOORWALL;
         break;
      case dr_lock1:
      case dr_lock2:
      case dr_lock3:
      case dr_lock4:
         doorpage = DOORWALL+6;
         break;
      case dr_elevator:
         doorpage = DOORWALL+4;
         break;
   }

   postsource = PM_GetTexture(doorpage) + texture;
}

/*
====================
=
= HitVertDoor
=
====================
*/

static void HitVertDoor (void)
{
   int doorpage;
   int doornum = tilehit&0x7f;
   int texture = ((yintercept - doorposition[doornum]) >> TEXTUREFROMFIXEDSHIFT) & TEXTUREMASK;

   if (lasttilehit == tilehit)
   {
      ScalePost();

      if((pixx&3) && texture == lasttexture)
      {
         postx            = pixx;
         wallheight[pixx] = wallheight[pixx-1];
         return;
      }

      wallheight[pixx]    = CalcHeight();
      postsource         += texture-lasttexture;
      postwidth           = 1;
      postx               = pixx;
      lasttexture         = texture;
      return;
   }

   if (lastside != -1)
      ScalePost();

   lastside         = 2;
   lasttilehit      = tilehit;
   lasttexture      = texture;
   wallheight[pixx] = CalcHeight();
   postx            = pixx;
   postwidth        = 1;

   switch(doorobjlist[doornum].lock)
   {
      case dr_normal:
         doorpage = DOORWALL+1;
         break;
      case dr_lock1:
      case dr_lock2:
      case dr_lock3:
      case dr_lock4:
         doorpage = DOORWALL+7;
         break;
      case dr_elevator:
         doorpage = DOORWALL+5;
         break;
   }

   postsource = PM_GetTexture(doorpage) + texture;
}

//==========================================================================

byte vgaCeiling[]=
{
#ifndef SPEAR
 0x1d,0x1d,0x1d,0x1d,0x1d,0x1d,0x1d,0x1d,0x1d,0xbf,
 0x4e,0x4e,0x4e,0x1d,0x8d,0x4e,0x1d,0x2d,0x1d,0x8d,
 0x1d,0x1d,0x1d,0x1d,0x1d,0x2d,0xdd,0x1d,0x1d,0x98,

 0x1d,0x9d,0x2d,0xdd,0xdd,0x9d,0x2d,0x4d,0x1d,0xdd,
 0x7d,0x1d,0x2d,0x2d,0xdd,0xd7,0x1d,0x1d,0x1d,0x2d,
 0x1d,0x1d,0x1d,0x1d,0xdd,0xdd,0x7d,0xdd,0xdd,0xdd
#else
 0x6f,0x4f,0x1d,0xde,0xdf,0x2e,0x7f,0x9e,0xae,0x7f,
 0x1d,0xde,0xdf,0xde,0xdf,0xde,0xe1,0xdc,0x2e,0x1d,0xdc
#endif
};

/*
=====================
=
= VGAClearScreen
=
=====================
*/

static void VGAClearScreen (void)
{
   int y;
   byte ceiling = vgaCeiling[gamestate.episode*10+mapon];
   byte *ptr    = vbuf;

   for(y = 0; y < viewheight / 2; y++, ptr += vbufPitch)
      memset(ptr, ceiling, viewwidth);

   for(; y < viewheight; y++, ptr += vbufPitch)
      memset(ptr, 0x19, viewwidth);
}

//==========================================================================

/*
=====================
=
= CalcRotate
=
=====================
*/

static int CalcRotate (objtype *ob)
{
   int angle;

   /* this isn't exactly correct, as it should 
    * vary by a trig value, but it is close 
    * enough with only eight rotations. */

   int viewangle = player->angle + (centerx - ob->viewx)/8;

   if (ob->obclass == rocketobj || ob->obclass == hrocketobj)
      angle = (viewangle-180) - ob->angle;
   else
      angle = (viewangle-180) - dirangle[ob->dir];

   angle += ANGLES/16;
   while (angle >= ANGLES)
      angle -= ANGLES;
   while (angle < 0)
      angle += ANGLES;

   /* 2 rotation pain frame */
   if (ob->state->rotate == 2)
      return 0; /* pain with shooting frame bugfix */

   return angle/(ANGLES/8);
}

static void ScaleShape (int xcenter, int shapenum, unsigned height, uint32_t flags)
{
   unsigned scale, pixheight;
   unsigned starty,endy;
   word *cmdptr;
   byte *cline;
   byte *line;
   byte *vmem;
   int actx,i,upperedge;
   short newstart;
   int scrstarty,screndy,lpix,rpix,pixcnt,ycnt;
   unsigned j;
   byte col;
   t_compshape *shape = (t_compshape *) PM_GetSprite(shapenum);
   word leftpix       = (word)Retro_SwapLES16(shape->leftpix);
   word rightpix      = (word)Retro_SwapLES16(shape->rightpix);
   scale              = height >> 3; /* low three bits are fractional */

   /* too close or far away? */
   if(!scale)
      return;   

   pixheight          = scale * SPRITESCALEFACTOR;
   actx               = xcenter-scale;
   upperedge          = viewheight/2-scale;

   cmdptr             = (word *)shape->dataofs;

   for(i= leftpix, pixcnt= i * pixheight, rpix = (pixcnt >> 6) + actx;
         i <= rightpix;
         i++, cmdptr++)
   {
      lpix=rpix;

      if(lpix >= viewwidth)
         break;

      pixcnt += pixheight;
      rpix    = (pixcnt >> 6) + actx;

      if(lpix != rpix && rpix > 0)
      {
         if(lpix < 0)
            lpix=0;

         if(rpix > viewwidth)
            rpix= viewwidth, i = rightpix + 1;

         cline = (byte *)shape + (word)Retro_SwapLES16(*cmdptr);

         while(lpix < rpix)
         {
            if(wallheight[lpix] <= (int)height)
            {
               line = cline;

               while((endy = READWORD(&line)) != 0)
               {
                  endy     >>= 1;
                  newstart   = READWORD(&line);
                  starty     = READWORD(&line) >> 1;
                  ycnt       = starty * pixheight;
                  screndy    = (ycnt >> 6) + upperedge;

                  if(screndy<0)
                     vmem    = vbuf+lpix;
                  else
                     vmem    = vbuf + screndy * vbufPitch + lpix;

                  for(j = starty; j < endy; j++)
                  {
                     scrstarty  = screndy;
                     ycnt      += pixheight;
                     screndy    = (ycnt>>6) + upperedge;

                     if(scrstarty != screndy && screndy > 0)
                     {
                        col=((byte *)shape)[newstart+j];

                        if(scrstarty < 0)
                           scrstarty=0;

                        if(screndy > viewheight)
                           screndy=viewheight,j=endy;

                        while(scrstarty < screndy)
                        {
                           *vmem=col;
                           vmem+=vbufPitch;
                           scrstarty++;
                        }
                     }
                  }
               }
            }
            lpix++;
         }
      }
   }
}

static void SimpleScaleShape (int xcenter, int shapenum, unsigned height)
{
   unsigned starty,endy;
   byte *cline;
   byte *line;
   int i;
   short newstart;
   int scrstarty,screndy,lpix,rpix,pixcnt,ycnt;
   unsigned j;
   byte col;
   byte *vmem;
   t_compshape *shape = (t_compshape *) PM_GetSprite(shapenum);
   word leftpix       = (word)Retro_SwapLES16(shape->leftpix);
   word rightpix      = (word)Retro_SwapLES16(shape->rightpix);
   unsigned scale     = height >> 1;
   unsigned pixheight = scale * SPRITESCALEFACTOR;
   int actx           = xcenter - scale;
   int upperedge      = viewheight / 2 - scale;
   word *cmdptr       = shape->dataofs;

   for(i = leftpix, pixcnt = i * pixheight, rpix = (pixcnt >> 6) + actx;
         i <= rightpix;
         i++, cmdptr++)
   {
      lpix      = rpix;

      if(lpix >= viewwidth)
         break;

      pixcnt   += pixheight;
      rpix      = (pixcnt>>6)+actx;

      if (lpix == rpix)
         continue;
      if (rpix <= 0)
         continue;

      if(lpix < 0)
         lpix = 0;
      if(rpix > viewwidth)
         rpix = viewwidth, i = rightpix + 1;
      cline   = (byte *)shape + (word)Retro_SwapLES16(*cmdptr);

      while(lpix < rpix)
      {
         line=cline;

         while((endy = READWORD(&line)) != 0)
         {
            endy     >>= 1;
            newstart   = READWORD(&line);
            starty     = READWORD(&line) >> 1;
            ycnt       = starty * pixheight;
            screndy    = (ycnt>>6)+upperedge;

            if(screndy<0)
               vmem    = vbuf+lpix;
            else
               vmem    = vbuf+screndy*vbufPitch+lpix;

            for(j = starty; j < endy; j++)
            {
               scrstarty  = screndy;
               ycnt      += pixheight;
               screndy    = (ycnt >> 6) + upperedge;

               if(scrstarty != screndy && screndy > 0)
               {
                  col = ((byte *)shape)[newstart+j];

                  if (scrstarty < 0)
                     scrstarty = 0;
                  if (screndy > viewheight)
                     screndy=viewheight,j=endy;

                  while(scrstarty < screndy)
                  {
                     *vmem=col;
                     vmem+=vbufPitch;
                     scrstarty++;
                  }
               }
            }
         }
         lpix++;
      }
   }
}

#define MAXVISABLE 250

typedef struct
{
   short      viewx;
   short      viewheight;
   short      shapenum;
   /* this must be changed to uint32_t, when you need more than 16-flags for drawing */
   short      flags;          
} visobj_t;

visobj_t vislist[MAXVISABLE];
visobj_t *visptr,*visstep,*farthest;

/*
=====================
=
= DrawScaleds
=
= Draws all objects that are visable
=
=====================
*/

static void DrawScaleds (void)
{
   int      i,least,numvisable,height;
   byte     *tilespot,*visspot;
   unsigned spotloc;
   statobj_t *statptr;
   objtype   *obj;

   visptr = &vislist[0];

   /* place static objects */
   for (statptr = &statobjlist[0] ; statptr !=laststatobj ; statptr++)
   {
      /* object has been deleted? */
      if ((visptr->shapenum = statptr->shapenum) == -1)
         continue; 
      
      /* not visable? */
      if (!*statptr->visspot)
         continue; 

      if (TransformTile (statptr->tilex,statptr->tiley,
               &visptr->viewx,&visptr->viewheight) && statptr->flags & FL_BONUS)
      {
         GetBonus (statptr);

         /* object has been taken? */
         if(statptr->shapenum == -1)
            continue;
      }

      /* too close to the object? */
      if (!visptr->viewheight)
         continue;

      /* don't let it overflow */
      if (visptr < &vislist[MAXVISABLE-1])
      {
         visptr->flags = (short) statptr->flags;
         visptr++;
      }
   }

   /* place active objects */
   for (obj = player->next;obj;obj=obj->next)
   {
      /* no shape? */
      if ((visptr->shapenum = obj->state->shapenum)==0)
         continue;

      spotloc  = (obj->tilex<<mapshift)+obj->tiley;   // optimize: keep in struct?
      visspot  = &spotvis[0][0]+spotloc;
      tilespot = &tilemap[0][0]+spotloc;

      /* could be in any of the nine surrounding tiles */
      if (*visspot
            || ( *(visspot-1) && !*(tilespot-1) )
            || ( *(visspot+1) && !*(tilespot+1) )
            || ( *(visspot-65) && !*(tilespot-65) )
            || ( *(visspot-64) && !*(tilespot-64) )
            || ( *(visspot-63) && !*(tilespot-63) )
            || ( *(visspot+65) && !*(tilespot+65) )
            || ( *(visspot+64) && !*(tilespot+64) )
            || ( *(visspot+63) && !*(tilespot+63) ) )
      {
         obj->active = ac_yes;
         TransformActor (obj);

         /* too close or far away? */
         if (!obj->viewheight)
            continue;

         visptr->viewx       = obj->viewx;
         visptr->viewheight  = obj->viewheight;

         /* special shape? */
         if (visptr->shapenum == -1)
            visptr->shapenum = obj->temp1;

         if (obj->state->rotate)
            visptr->shapenum += CalcRotate (obj);

         /* don't let it overflow. */
         if (visptr < &vislist[MAXVISABLE-1])
         {
            visptr->flags = (short) obj->flags;

            visptr++;
         }
         obj->flags |= FL_VISABLE;
      }
      else
         obj->flags &= ~FL_VISABLE;
   }

   /* draw from back to front */
   numvisable = (int) (visptr-&vislist[0]);

   /* no visable objects? */
   if (!numvisable)
      return;                                                                 

   for (i = 0; i < numvisable; i++)
   {
      least = 32000;
      for (visstep = &vislist[0]; visstep<visptr; visstep++)
      {
         height = visstep->viewheight;
         if (height < least)
         {
            least = height;
            farthest = visstep;
         }
      }

      /* draw farthest */
      ScaleShape(farthest->viewx, farthest->shapenum,
            farthest->viewheight, farthest->flags);

      farthest->viewheight = 32000;
   }
}

/*
==============
=
= DrawPlayerWeapon
=
= Draw the player's hands
=
==============
*/

int weaponscale[NUMWEAPONS] = {SPR_KNIFEREADY, SPR_PISTOLREADY,
    SPR_MACHINEGUNREADY, SPR_CHAINREADY};

static void DrawPlayerWeapon (void)
{
    int shapenum;

#ifndef SPEAR
    if (gamestate.victoryflag)
    {
#ifndef APOGEE_1_0
        if (player->state == &s_deathcam && (GetTimeCount()&32) )
            SimpleScaleShape(viewwidth/2,SPR_DEATHCAM,viewheight+1);
#endif
        return;
    }
#endif

    if (gamestate.weapon != -1)
    {
        shapenum = weaponscale[gamestate.weapon]+gamestate.weaponframe;
        SimpleScaleShape(viewwidth/2,shapenum,viewheight+1);
    }

    if (demorecord || demoplayback)
        SimpleScaleShape(viewwidth/2,SPR_DEMO,viewheight+1);
}


//==========================================================================


/*
=====================
=
= CalcTics
=
=====================
*/

#define MAXTICS 10

void CalcTics (void)
{
   uint32_t curtime;

   /* calculate tics since last refresh for adaptive timing */
   if (lasttimecount > (int32_t) GetTimeCount())
      lasttimecount = GetTimeCount();    /* if the game was paused a LONG time */

   curtime = LR_GetTicks();
   tics = (curtime * 7) / 100 - lasttimecount;

   if(!tics)
   {
      /* wait until end of current tic */
      rarch_sleep(((lasttimecount + 1) * 100) / 7 - curtime);
      tics = 1;
   }

   lasttimecount += tics;

   if (tics > MAXTICS)
      tics = MAXTICS;
}

static void AsmRefresh(void)
{
   int32_t xstep,ystep;
   longword xpartial,ypartial;
   boolean playerInPushwallBackTile = tilemap[focaltx][focalty] == 64;

   for(pixx = 0; pixx < viewwidth; pixx++)
   {
      short angl = midangle+pixelangle[pixx];
      if(angl < 0)
         angl += FINEANGLES;
      if(angl >= 3600)
         angl -= FINEANGLES;

      if(angl < 900)
      {
         xtilestep=1;
         ytilestep=-1;
         xstep=finetangent[900-1-angl];
         ystep=-finetangent[angl];
         xpartial=xpartialup;
         ypartial=ypartialdown;
      }
      else if(angl < 1800)
      {
         xtilestep=-1;
         ytilestep=-1;
         xstep=-finetangent[angl-900];
         ystep=-finetangent[1800-1-angl];
         xpartial=xpartialdown;
         ypartial=ypartialdown;
      }
      else if(angl < 2700)
      {
         xtilestep= -1;
         ytilestep=  1;
         xstep    = -finetangent[2700-1-angl];
         ystep    = finetangent[angl-1800];
         xpartial = xpartialdown;
         ypartial = ypartialup;
      }
      else if(angl < 3600)
      {
         xtilestep= 1;
         ytilestep= 1;
         xstep    = finetangent[angl-2700];
         ystep    = finetangent[3600-1-angl];
         xpartial = xpartialup;
         ypartial = ypartialup;
      }

      yintercept  = FixedMul(ystep,xpartial)+viewy;
      xtile       = focaltx+xtilestep;
      xspot       = (word)((xtile<<mapshift)+((uint32_t)yintercept>>16));
      xintercept  = FixedMul(xstep,ypartial)+viewx;
      ytile       = focalty+ytilestep;
      yspot       = (word)((((uint32_t)xintercept>>16)<<mapshift)+ytile);
      texdelta    = 0;

      /* Special treatment when player is in back tile of pushwall */
      if(playerInPushwallBackTile)
      {
         if(    pwalldir == di_east && xtilestep ==  1
               || pwalldir == di_west && xtilestep == -1)
         {
            int32_t yintbuf = yintercept - ((ystep * (64 - pwallpos)) >> 6);

            /* ray hits pushwall back? */
            if((yintbuf >> 16) == focalty)
            {
               if(pwalldir == di_east)
                  xintercept = (focaltx << TILESHIFT) + (pwallpos << 10);
               else
                  xintercept = (focaltx << TILESHIFT) - TILEGLOBAL + ((64 - pwallpos) << 10);
               yintercept = yintbuf;
               ytile = (short) (yintercept >> TILESHIFT);
               tilehit = pwalltile;
               HitVertWall();
               continue;
            }
         }
         else if(pwalldir == di_south && ytilestep ==  1
               ||  pwalldir == di_north && ytilestep == -1)
         {
            int32_t xintbuf = xintercept - ((xstep * (64 - pwallpos)) >> 6);

            /* ray hits pushwall back? */
            if((xintbuf >> 16) == focaltx)
            {
               xintercept = xintbuf;
               if(pwalldir == di_south)
                  yintercept = (focalty << TILESHIFT) + (pwallpos << 10);
               else
                  yintercept = (focalty << TILESHIFT) - TILEGLOBAL + ((64 - pwallpos) << 10);
               xtile = (short) (xintercept >> TILESHIFT);
               tilehit = pwalltile;
               HitHorizWall();
               continue;
            }
         }
      }

      do
      {
         if(ytilestep==-1 && (yintercept>>16)<=ytile)
            goto horizentry;
         if(ytilestep==1 && (yintercept>>16)>=ytile)
            goto horizentry;
vertentry:
         if((uint32_t)yintercept>mapheight*65536-1 || (word)xtile>=mapwidth)
         {
            if (xtile<0)
               xintercept=0, xtile=0;
            else if(xtile>=mapwidth)
               xintercept=mapwidth<<TILESHIFT, xtile=mapwidth-1;
            else
               xtile=(short) (xintercept >> TILESHIFT);

            if(yintercept<0)
               yintercept=0, ytile=0;
            else if(yintercept>=(mapheight<<TILESHIFT))
               yintercept=mapheight<<TILESHIFT, ytile=mapheight-1;

            yspot=0xffff;
            tilehit=0;
            HitHorizWall();
            break;
         }

         if(xspot>=maparea)
            break;

         tilehit=((byte *)tilemap)[xspot];

         if(tilehit)
         {
            if(tilehit & 0x80)
            {
               int32_t yintbuf=yintercept+(ystep>>1);
               if((yintbuf>>16)!=(yintercept>>16))
                  goto passvert;
               if((word)yintbuf<doorposition[tilehit&0x7f])
                  goto passvert;
               yintercept=yintbuf;
               xintercept=(xtile<<TILESHIFT)|0x8000;
               ytile = (short) (yintercept >> TILESHIFT);
               HitVertDoor();
            }
            else
            {
               if(tilehit == 64)
               {
                  if(pwalldir==di_west || pwalldir==di_east)
                  {
                     int32_t yintbuf;
                     int pwallposnorm = pwallpos;
                     int pwallposinv  = 64 - pwallpos;

                     if(pwalldir == di_west)
                     {
                        pwallposnorm = 64 - pwallpos;
                        pwallposinv = pwallpos;
                     }

                     if(pwalldir == di_east && xtile==pwallx && ((uint32_t)yintercept>>16)==pwally
                           || pwalldir == di_west && !(xtile==pwallx && ((uint32_t)yintercept>>16)==pwally))
                     {
                        yintbuf=yintercept+((ystep*pwallposnorm)>>6);
                        if((yintbuf>>16) != (yintercept>>16))
                           goto passvert;

                        xintercept=(xtile<<TILESHIFT)+TILEGLOBAL-(pwallposinv<<10);
                     }
                     else
                     {
                        yintbuf=yintercept+((ystep*pwallposinv)>>6);
                        if((yintbuf>>16)!=(yintercept>>16))
                           goto passvert;

                        xintercept=(xtile<<TILESHIFT)-(pwallposinv<<10);
                     }

                     yintercept=yintbuf;
                     ytile = (short) (yintercept >> TILESHIFT);
                     tilehit = pwalltile;
                     HitVertWall();
                  }
                  else
                  {
                     int pwallposi = pwallpos;

                     if(pwalldir == di_north)
                        pwallposi = 64-pwallpos;

                     if(pwalldir==di_south && (word)yintercept<(pwallposi<<10)
                           || pwalldir==di_north && (word)yintercept>(pwallposi<<10))
                     {
                        if(((uint32_t)yintercept>>16)==pwally && xtile==pwallx)
                        {
                           if(pwalldir==di_south && (int32_t)((word)yintercept)+ystep<(pwallposi<<10)
                                 || pwalldir==di_north && (int32_t)((word)yintercept)+ystep>(pwallposi<<10))
                              goto passvert;

                           if(pwalldir==di_south)
                              yintercept=(yintercept&0xffff0000)+(pwallposi<<10);
                           else
                              yintercept=(yintercept&0xffff0000)-TILEGLOBAL+(pwallposi<<10);
                           xintercept=xintercept-((xstep*(64-pwallpos))>>6);
                           xtile = (short) (xintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitHorizWall();
                        }
                        else
                        {
                           texdelta = -(pwallposi<<10);
                           xintercept=xtile<<TILESHIFT;
                           ytile = (short) (yintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitVertWall();
                        }
                     }
                     else
                     {
                        if(((uint32_t)yintercept>>16)==pwally && xtile==pwallx)
                        {
                           texdelta = -(pwallposi<<10);
                           xintercept=xtile<<TILESHIFT;
                           ytile = (short) (yintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitVertWall();
                        }
                        else
                        {
                           if(pwalldir==di_south && (int32_t)((word)yintercept)+ystep>(pwallposi<<10)
                                 || pwalldir==di_north && (int32_t)((word)yintercept)+ystep<(pwallposi<<10))
                              goto passvert;

                           if(pwalldir==di_south)
                              yintercept = (yintercept&0xffff0000)-((64-pwallpos)<<10);
                           else
                              yintercept = (yintercept&0xffff0000)+((64-pwallpos)<<10);
                           xintercept    =  xintercept-((xstep*pwallpos)>>6);
                           xtile         = (short) (xintercept >> TILESHIFT);
                           tilehit       = pwalltile;
                           HitHorizWall();
                        }
                     }
                  }
               }
               else
               {
                  xintercept = xtile<<TILESHIFT;
                  ytile      = (short) (yintercept >> TILESHIFT);
                  HitVertWall();
               }
            }
            break;
         }
passvert:
         *((byte *)spotvis+xspot)=1;
         xtile+=xtilestep;
         yintercept+=ystep;
         xspot=(word)((xtile<<mapshift)+((uint32_t)yintercept>>16));
      }while(1);
      continue;

      do
      {
         if(xtilestep==-1 && (xintercept>>16)<=xtile)
            goto vertentry;
         if(xtilestep==1 && (xintercept>>16)>=xtile)
            goto vertentry;
horizentry:
         if((uint32_t)xintercept>mapwidth*65536-1 || (word)ytile>=mapheight)
         {
            if (ytile<0)
               yintercept=0, ytile=0;
            else if(ytile >= mapheight)
               yintercept = mapheight<<TILESHIFT, ytile=mapheight-1;
            else
               ytile=(short) (yintercept >> TILESHIFT);

            if(xintercept<0)
               xintercept=0, xtile=0;
            else if(xintercept>=(mapwidth<<TILESHIFT))
               xintercept=mapwidth<<TILESHIFT, xtile=mapwidth-1;
            xspot=0xffff;
            tilehit=0;
            HitVertWall();
            break;
         }

         if(yspot>=maparea)
            break;
         tilehit=((byte *)tilemap)[yspot];

         if(tilehit)
         {
            if(tilehit&0x80)
            {
               int32_t xintbuf=xintercept+(xstep>>1);
               if((xintbuf>>16)!=(xintercept>>16))
                  goto passhoriz;
               if((word)xintbuf<doorposition[tilehit&0x7f])
                  goto passhoriz;
               xintercept=xintbuf;
               yintercept=(ytile<<TILESHIFT)+0x8000;
               xtile = (short) (xintercept >> TILESHIFT);
               HitHorizDoor();
            }
            else
            {
               if(tilehit==64)
               {
                  if(pwalldir==di_north || pwalldir==di_south)
                  {
                     int32_t xintbuf;
                     int pwallposnorm = pwallpos;
                     int pwallposinv  = 64 - pwallpos;

                     if(pwalldir==di_north)
                     {
                        pwallposnorm = 64-pwallpos;
                        pwallposinv = pwallpos;
                     }

                     if(pwalldir == di_south && ytile==pwally && ((uint32_t)xintercept>>16)==pwallx
                           || pwalldir == di_north && !(ytile==pwally && ((uint32_t)xintercept>>16)==pwallx))
                     {
                        xintbuf=xintercept+((xstep*pwallposnorm)>>6);
                        if((xintbuf>>16)!=(xintercept>>16))
                           goto passhoriz;

                        yintercept=(ytile<<TILESHIFT)+TILEGLOBAL-(pwallposinv<<10);
                     }
                     else
                     {
                        xintbuf=xintercept+((xstep*pwallposinv)>>6);
                        if((xintbuf>>16)!=(xintercept>>16))
                           goto passhoriz;

                        yintercept=(ytile<<TILESHIFT)-(pwallposinv<<10);
                     }

                     xintercept=xintbuf;
                     xtile = (short) (xintercept >> TILESHIFT);
                     tilehit=pwalltile;
                     HitHorizWall();
                  }
                  else
                  {
                     int pwallposi = pwallpos;
                     if(pwalldir==di_west) pwallposi = 64-pwallpos;
                     if(pwalldir==di_east && (word)xintercept<(pwallposi<<10)
                           || pwalldir==di_west && (word)xintercept>(pwallposi<<10))
                     {
                        if(((uint32_t)xintercept>>16)==pwallx && ytile==pwally)
                        {
                           if(pwalldir==di_east && (int32_t)((word)xintercept)+xstep<(pwallposi<<10)
                                 || pwalldir==di_west && (int32_t)((word)xintercept)+xstep>(pwallposi<<10))
                              goto passhoriz;

                           if(pwalldir==di_east)
                              xintercept=(xintercept&0xffff0000)+(pwallposi<<10);
                           else
                              xintercept=(xintercept&0xffff0000)-TILEGLOBAL+(pwallposi<<10);
                           yintercept=yintercept-((ystep*(64-pwallpos))>>6);
                           ytile = (short) (yintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitVertWall();
                        }
                        else
                        {
                           texdelta = -(pwallposi<<10);
                           yintercept=ytile<<TILESHIFT;
                           xtile = (short) (xintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitHorizWall();
                        }
                     }
                     else
                     {
                        if(((uint32_t)xintercept>>16)==pwallx && ytile==pwally)
                        {
                           texdelta = -(pwallposi<<10);
                           yintercept=ytile<<TILESHIFT;
                           xtile = (short) (xintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitHorizWall();
                        }
                        else
                        {
                           if(pwalldir==di_east && (int32_t)((word)xintercept)+xstep>(pwallposi<<10)
                                 || pwalldir==di_west && (int32_t)((word)xintercept)+xstep<(pwallposi<<10))
                              goto passhoriz;

                           if(pwalldir==di_east)
                              xintercept=(xintercept&0xffff0000)-((64-pwallpos)<<10);
                           else
                              xintercept=(xintercept&0xffff0000)+((64-pwallpos)<<10);
                           yintercept=yintercept-((ystep*pwallpos)>>6);
                           ytile = (short) (yintercept >> TILESHIFT);
                           tilehit=pwalltile;
                           HitVertWall();
                        }
                     }
                  }
               }
               else
               {
                  yintercept=ytile<<TILESHIFT;
                  xtile = (short) (xintercept >> TILESHIFT);
                  HitHorizWall();
               }
            }
            break;
         }
passhoriz:
         *((byte *)spotvis+yspot)=1;
         ytile+=ytilestep;
         xintercept+=xstep;
         yspot=(word)((((uint32_t)xintercept>>16)<<mapshift)+ytile);
      }
      while(1);
   }
}

/*
====================
=
= WallRefresh
=
====================
*/

static void WallRefresh(void)
{
   xpartialdown = viewx&(TILEGLOBAL-1);
   xpartialup = TILEGLOBAL-xpartialdown;
   ypartialdown = viewy&(TILEGLOBAL-1);
   ypartialup = TILEGLOBAL-ypartialdown;

   min_wallheight = viewheight;
   lastside = -1;                  /* the first pixel is on a new wall */
   AsmRefresh ();
   ScalePost ();                   /* no more optimization on last post */
}

static void CalcViewVariables(void)
{
   viewangle = player->angle;

#if 0
   printf("\nvieangle=%d\n",viewangle);
#endif

   midangle  = viewangle*(FINEANGLES/ANGLES);
   viewsin   = sintable[viewangle];
   viewcos   = costable[viewangle];

#if 0
   printf("%d\n",viewcos);
#endif
   
   viewx     = player->x - FixedMul(focallength,viewcos);
   viewy     = player->y + FixedMul(focallength,viewsin);

   focaltx   = (short)(viewx>>TILESHIFT);
   focalty   = (short)(viewy>>TILESHIFT);

   viewtx    = (short)(player->x >> TILESHIFT);
   viewty    = (short)(player->y >> TILESHIFT);
}

//==========================================================================

/*
========================
=
= ThreeDRefresh
=
========================
*/

void ThreeDRefresh (void)
{
   /* clear out the traced array */
   memset(spotvis,0,maparea);

   /* Detect all sprites over player fix */
   spotvis[player->tilex][player->tiley] = 1;

   vbuf       = VL_LockSurface(screenBuffer);
   vbuf      += screenofs;
   vbufPitch  = bufferPitch;

   CalcViewVariables();

   /* follow the walls from there to the right, drawing as we go */
   VGAClearScreen ();

   WallRefresh ();

   /* draw all the scaled images */
   DrawScaleds();          /* draw scaled stuff */
   DrawPlayerWeapon ();    /* draw player's hands */

   if(Keyboard[sc_Tab] && viewsize == 21 && gamestate.weapon != -1)
      ShowActStatus();

   VL_UnlockSurface(screenBuffer);
   vbuf = NULL;

   /* show screen and time last cycle */
   if (fizzlein)
   {
      FizzleFade(screenBuffer, 0, 0, screenWidth, screenHeight, 20, false);
      fizzlein = false;

      lasttimecount = GetTimeCount();          // don't make a big tic count
   }
   else
      VH_UpdateScreen();
}
