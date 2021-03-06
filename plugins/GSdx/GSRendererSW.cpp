/*
 *	Copyright (C) 2007-2009 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "GSRendererSW.h"

const GSVector4 g_pos_scale(1.0f / 16, 1.0f / 16, 1.0f, 128.0f);

GSRendererSW::GSRendererSW(int threads)
	: m_fzb(NULL)
{
	InitVertexKick(GSRendererSW);

	m_tc = new GSTextureCacheSW(this);

	memset(m_texture, 0, sizeof(m_texture));

	m_rl = GSRasterizerList::Create<GSDrawScanline>(threads, &m_perfmon);

	m_output = (uint8*)_aligned_malloc(1024 * 1024 * sizeof(uint32), 32);

	memset(m_fzb_pages, 0, sizeof(m_fzb_pages));
	memset(m_tex_pages, 0, sizeof(m_tex_pages));
}

GSRendererSW::~GSRendererSW()
{
	delete m_tc;

	for(int i = 0; i < countof(m_texture); i++)
	{
		delete m_texture[i];
	}

	delete m_rl;

	_aligned_free(m_output);
}

void GSRendererSW::Reset()
{
	// TODO: GSreset can come from the main thread too => crash
	// m_tc->RemoveAll();

	m_reset = true;

	GSRendererT<GSVertexSW>::Reset();
}

void GSRendererSW::VSync(int field)
{
	Sync(0); // IncAge might delete a cached texture in use

	/*
	int draw[8], sum = 0;

	for(int i = 0; i < countof(draw); i++)
	{
		draw[i] = m_perfmon.CPU(GSPerfMon::WorkerDraw0 + i);
		sum += draw[i];
	}

	printf("CPU %d Sync %d W %d %d %d %d %d %d %d %d (%d)\n",
		m_perfmon.CPU(GSPerfMon::Main),
		m_perfmon.CPU(GSPerfMon::Sync),
		draw[0], draw[1], draw[2], draw[3], draw[4], draw[5], draw[6], draw[7], sum);

	//
	printf("m_sync_count = %d\n", ((GSRasterizerList*)m_rl)->m_sync_count); ((GSRasterizerList*)m_rl)->m_sync_count = 0;
	printf("m_syncpoint_count = %d\n", ((GSRasterizerList*)m_rl)->m_syncpoint_count); ((GSRasterizerList*)m_rl)->m_syncpoint_count = 0;
	printf("m_solidrect_count = %d\n", ((GSRasterizerList*)m_rl)->m_solidrect_count); ((GSRasterizerList*)m_rl)->m_solidrect_count = 0;
*/	
	GSRendererT<GSVertexSW>::VSync(field);

	m_tc->IncAge();

	if(m_reset)
	{
		m_tc->RemoveAll();

		m_reset = false;
	}

	// if((m_perfmon.GetFrame() & 255) == 0) m_rl.PrintStats();
}

void GSRendererSW::ResetDevice()
{
	for(int i = 0; i < countof(m_texture); i++)
	{
		delete m_texture[i];

		m_texture[i] = NULL;
	}
}

GSTexture* GSRendererSW::GetOutput(int i)
{
	Sync(1);

	const GSRegDISPFB& DISPFB = m_regs->DISP[i].DISPFB;

	int w = DISPFB.FBW * 64;
	int h = GetFrameRect(i).bottom;

	// TODO: round up bottom

	if(m_dev->ResizeTexture(&m_texture[i], w, h))
	{
		static int pitch = 1024 * 4;

		GSVector4i r(0, 0, w, h);

		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[DISPFB.PSM];

		(m_mem.*psm.rtx)(m_mem.GetOffset(DISPFB.Block(), DISPFB.FBW, DISPFB.PSM), r.ralign<Align_Outside>(psm.bs), m_output, pitch, m_env.TEXA);

		m_texture[i]->Update(r, m_output, pitch);

		if(s_dump)
		{
			if(s_save && s_n >= s_saven)
			{
				m_texture[i]->Save(format("c:\\temp1\\_%05d_f%lld_fr%d_%05x_%d.bmp", s_n, m_perfmon.GetFrame(), i, (int)DISPFB.Block(), (int)DISPFB.PSM));
			}

			s_n++;
		}
	}

	return m_texture[i];
}

void GSRendererSW::Draw()
{
	if(m_dump) m_dump.Object(m_vertices, m_count, m_vt.m_primclass);

	GSVector4i scissor = GSVector4i(m_context->scissor.in);
	GSVector4i bbox = GSVector4i(m_vt.m_min.p.floor().xyxy(m_vt.m_max.p.ceil()));

	scissor.z = std::min<int>(scissor.z, (int)m_context->FRAME.FBW * 64); // TODO: find a game that overflows and check which one is the right behaviour
	
	GSVector4i r = bbox.rintersect(scissor);

	shared_ptr<GSRasterizerData> data(new GSRasterizerData2(this));

	GSRasterizerData2* data2 = (GSRasterizerData2*)data.get();

	if(!GetScanlineGlobalData(data2))
	{
		return;
	}

	GSScanlineGlobalData* gd = (GSScanlineGlobalData*)data->param;

	data->scissor = scissor;
	data->bbox = bbox;
	data->primclass = m_vt.m_primclass;
	data->vertices = (GSVertexSW*)_aligned_malloc(sizeof(GSVertexSW) * m_count, 16); // TODO: detach m_vertices and reallocate later?
	memcpy(data->vertices, m_vertices, sizeof(GSVertexSW) * m_count); // TODO: m_vt.Update fetches all the vertices already, could also store them here
	data->count = m_count;
	data->solidrect = gd->sel.IsSolidRect();
	data->frame = m_perfmon.GetFrame();

	//

	vector<uint32>* fb_pages = NULL;
	vector<uint32>* zb_pages = NULL;

	if(gd->sel.fwrite)
	{
		fb_pages = m_context->offset.fb->GetPages(r);

		m_tc->InvalidatePages(fb_pages, m_context->offset.fb->psm);
	}

	if(gd->sel.zwrite)
	{
		zb_pages = m_context->offset.zb->GetPages(r);

		m_tc->InvalidatePages(zb_pages, m_context->offset.zb->psm);
	}

	// set data->syncpoint

	if(m_fzb != m_context->offset.fzb)
	{
		m_fzb = m_context->offset.fzb;

		data->syncpoint = true;
	}

	// - chross-check frame and z-buffer pages, they cannot overlap with eachother and with previous batches in queue
	// - m_fzb filters out most of these cases, only have to be careful when the addresses stay the same and the output is mutually enabled/disabled and alternating (Bully FBP/ZBP = 0x2300)

	if(!data->syncpoint)
	{
		if(gd->sel.fwrite)
		{
			for(vector<uint32>::iterator i = fb_pages->begin(); i != fb_pages->end(); i++)
			{
				if(m_fzb_pages[*i] & 0xffff0000) // already used as a z-buffer
				{
					data->syncpoint = true;
					
					break;
				}
			}
		}
	}

	if(!data->syncpoint)
	{
		if(gd->sel.zwrite)
		{
			for(vector<uint32>::iterator i = zb_pages->begin(); i != zb_pages->end(); i++)
			{
				if(m_fzb_pages[*i] & 0x0000ffff) // already used as a frame buffer
				{
					data->syncpoint = true;

					break;
				}
			}
		}
	}

	//

	data2->UseTargetPages(fb_pages, zb_pages);

	//

	if(s_dump)
	{
		Sync(3);

		uint64 frame = m_perfmon.GetFrame();

		string s;

		if(s_save && s_n >= s_saven && PRIM->TME)
		{
			s = format("c:\\temp1\\_%05d_f%lld_tex_%05x_%d.bmp", s_n, frame, (int)m_context->TEX0.TBP0, (int)m_context->TEX0.PSM);

			m_mem.SaveBMP(s, m_context->TEX0.TBP0, m_context->TEX0.TBW, m_context->TEX0.PSM, 1 << m_context->TEX0.TW, 1 << m_context->TEX0.TH);
		}

		s_n++;

		if(s_save && s_n >= s_saven)
		{
			s = format("c:\\temp1\\_%05d_f%lld_rt0_%05x_%d.bmp", s_n, frame, m_context->FRAME.Block(), m_context->FRAME.PSM);

			m_mem.SaveBMP(s, m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM, GetFrameRect().width(), 512);
		}

		if(s_savez && s_n >= s_saven)
		{
			s = format("c:\\temp1\\_%05d_f%lld_rz0_%05x_%d.bmp", s_n, frame, m_context->ZBUF.Block(), m_context->ZBUF.PSM);

			m_mem.SaveBMP(s, m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM, GetFrameRect().width(), 512);
		}

		s_n++;

		m_rl->Queue(data);

		Sync(4);

		if(s_save && s_n >= s_saven)
		{
			s = format("c:\\temp1\\_%05d_f%lld_rt1_%05x_%d.bmp", s_n, frame, m_context->FRAME.Block(), m_context->FRAME.PSM);

			m_mem.SaveBMP(s, m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM, GetFrameRect().width(), 512);
		}

		if(s_savez && s_n >= s_saven)
		{
			s = format("c:\\temp1\\_%05d_f%lld_rz1_%05x_%d.bmp", s_n, frame, m_context->ZBUF.Block(), m_context->ZBUF.PSM);

			m_mem.SaveBMP(s, m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM, GetFrameRect().width(), 512);
		}

		s_n++;
	}
	else
	{
		m_rl->Queue(data);
	}

	int prims = 0;
	
	switch(data->primclass)
	{
	case GS_POINT_CLASS: prims = data->count; break;
	case GS_LINE_CLASS: prims = data->count / 2; break;
	case GS_TRIANGLE_CLASS: prims = data->count / 3; break;
	case GS_SPRITE_CLASS: prims = data->count / 2; break;
	}

	m_perfmon.Put(GSPerfMon::Prim, prims);

	/*
	if(0)//stats.ticks > 5000000)
	{
		printf("* [%lld | %012llx] ticks %lld prims %d (%d) pixels %d (%d)\n",
			m_perfmon.GetFrame(), gd->sel.key,
			stats.ticks,
			stats.prims, stats.prims > 0 ? (int)(stats.ticks / stats.prims) : -1,
			stats.pixels, stats.pixels > 0 ? (int)(stats.ticks / stats.pixels) : -1);
	}
	*/
}

void GSRendererSW::Sync(int reason)
{
	//printf("sync %d\n", reason);

	GSPerfMonAutoTimer pmat(&m_perfmon, GSPerfMon::Sync);

	m_rl->Sync();
}

void GSRendererSW::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	GSOffset* o = m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM);

	vector<uint32>* pages = o->GetPages(r);

	m_tc->InvalidatePages(pages, o->psm);

	// check if the changing pages either used as a texture or a target

	for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
	{
		uint32 page = *i;
		
		//while(m_fzb_pages[page] | m_tex_pages[page]) _mm_pause();

		if(m_fzb_pages[page] | m_tex_pages[page])
		{
			Sync(5);

			break;
		}
	}

	delete pages;
}

void GSRendererSW::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	GSOffset* o = m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM);

	vector<uint32>* pages = o->GetPages(r);

	for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
	{
		//while(m_fzb_pages[*i]) _mm_pause();

		if(m_fzb_pages[*i])
		{
			Sync(6);

			break;
		}
	}

	delete pages;
}

void GSRendererSW::UsePages(const vector<uint32>* pages, int type)
{
	if(type < 2)
	{
		for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
		{
			ASSERT(((short*)&m_fzb_pages[*i])[type] < SHRT_MAX);

			_InterlockedIncrement16((short*)&m_fzb_pages[*i] + type);
		}
	}
	else
	{
		for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
		{
			//while(m_fzb_pages[*i]) _mm_pause();

			if(m_fzb_pages[*i]) // currently being drawn to? => sync (could even spin and wait until it hits 0, not sure if it's worth though, or just create 512 condvars? :D)
			{
				Sync(7);

				break;
			}
		}

		for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
		{
			ASSERT(m_tex_pages[*i] < SHRT_MAX);

			_InterlockedIncrement16((short*)&m_tex_pages[*i]); // remember which texture pages are used
		}
	}
}

void GSRendererSW::ReleasePages(const vector<uint32>* pages, int type)
{
	if(type < 2)
	{
		for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
		{
			ASSERT(((short*)&m_fzb_pages[*i])[type] > 0);

			_InterlockedDecrement16((short*)&m_fzb_pages[*i] + type);
		}
	}
	else
	{
		for(vector<uint32>::const_iterator i = pages->begin(); i != pages->end(); i++)
		{
			ASSERT(m_tex_pages[*i] > 0);

			_InterlockedDecrement16((short*)&m_tex_pages[*i]);
		}
	}
}

#include "GSTextureSW.h"

bool GSRendererSW::GetScanlineGlobalData(GSRasterizerData2* data2)
{
	GSScanlineGlobalData& gd = *(GSScanlineGlobalData*)data2->param;

	const GSDrawingEnvironment& env = m_env;
	const GSDrawingContext* context = m_context;
	const GS_PRIM_CLASS primclass = m_vt.m_primclass;

	gd.vm = m_mem.m_vm8;

	gd.fbr = context->offset.fb->pixel.row;
	gd.zbr = context->offset.zb->pixel.row;
	gd.fbc = context->offset.fb->pixel.col[0];
	gd.zbc = context->offset.zb->pixel.col[0];
	gd.fzbr = context->offset.fzb->row;
	gd.fzbc = context->offset.fzb->col;

	gd.sel.key = 0;

	gd.sel.fpsm = 3;
	gd.sel.zpsm = 3;
	gd.sel.atst = ATST_ALWAYS;
	gd.sel.tfx = TFX_NONE;
	gd.sel.ababcd = 255;
	gd.sel.sprite = primclass == GS_SPRITE_CLASS ? 1 : 0;

	uint32 fm = context->FRAME.FBMSK;
	uint32 zm = context->ZBUF.ZMSK || context->TEST.ZTE == 0 ? 0xffffffff : 0;

	if(context->TEST.ZTE && context->TEST.ZTST == ZTST_NEVER)
	{
		fm = 0xffffffff;
		zm = 0xffffffff;
	}

	if(PRIM->TME)
	{
		m_mem.m_clut.Read32(context->TEX0, env.TEXA);
	}

	if(context->TEST.ATE)
	{
		if(!TryAlphaTest(fm, zm))
		{
			gd.sel.atst = context->TEST.ATST;
			gd.sel.afail = context->TEST.AFAIL;

			gd.aref = GSVector4i((int)context->TEST.AREF);

			switch(gd.sel.atst)
			{
			case ATST_LESS:
				gd.sel.atst = ATST_LEQUAL;
				gd.aref -= GSVector4i::x00000001();
				break;
			case ATST_GREATER:
				gd.sel.atst = ATST_GEQUAL;
				gd.aref += GSVector4i::x00000001();
				break;
			}
		}
	}

	bool fwrite = fm != 0xffffffff;
	bool ftest = gd.sel.atst != ATST_ALWAYS || context->TEST.DATE && context->FRAME.PSM != PSM_PSMCT24;

	bool zwrite = zm != 0xffffffff;
	bool ztest = context->TEST.ZTE && context->TEST.ZTST > ZTST_ALWAYS;
	/*
	printf("%05x %d %05x %d %05x %d %dx%d\n", 
		fwrite || ftest ? m_context->FRAME.Block() : 0xfffff, m_context->FRAME.PSM,
		zwrite || ztest ? m_context->ZBUF.Block() : 0xfffff, m_context->ZBUF.PSM,
		PRIM->TME ? m_context->TEX0.TBP0 : 0xfffff, m_context->TEX0.PSM, (int)m_context->TEX0.TW, (int)m_context->TEX0.TH);
	*/
	if(!fwrite && !zwrite) return false;

	gd.sel.fwrite = fwrite;
	gd.sel.ftest = ftest;

	if(fwrite || ftest)
	{
		gd.sel.fpsm = GSLocalMemory::m_psm[context->FRAME.PSM].fmt;

		if((primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS) && m_vt.m_eq.rgba != 0xffff)
		{
			gd.sel.iip = PRIM->IIP;
		}

		if(PRIM->TME)
		{
			gd.sel.tfx = context->TEX0.TFX;
			gd.sel.tcc = context->TEX0.TCC;
			gd.sel.fst = PRIM->FST;
			gd.sel.ltf = m_vt.IsLinear();

			if(GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
			{
				gd.sel.tlu = 1;

				gd.clut = (uint32*)_aligned_malloc(sizeof(uint32) * 256, 32); // FIXME: might address uninitialized data of the texture (0xCD) that is not in 0-15 range for 4-bpp formats

				memcpy(gd.clut, (const uint32*)m_mem.m_clut, sizeof(uint32) * GSLocalMemory::m_psm[context->TEX0.PSM].pal);
			}

			gd.sel.wms = context->CLAMP.WMS;
			gd.sel.wmt = context->CLAMP.WMT;

			if(gd.sel.tfx == TFX_MODULATE && gd.sel.tcc && m_vt.m_eq.rgba == 0xffff && m_vt.m_min.c.eq(GSVector4i(128)))
			{
				// modulate does not do anything when vertex color is 0x80

				gd.sel.tfx = TFX_DECAL;
			}

			GSTextureCacheSW::Texture* t = m_tc->Lookup(context->TEX0, env.TEXA);

			if(t == NULL) {ASSERT(0); return false;}

			data2->UseSourcePages(t, 0);

			GSVector4i r;

			GetTextureMinMax(r, context->TEX0, context->CLAMP, gd.sel.ltf);

			if(!t->Update(r)) {ASSERT(0); return false;}

			if(s_dump)// && m_context->TEX1.MXL > 0 && m_context->TEX1.MMIN >= 2 && m_context->TEX1.MMIN <= 5 && m_vt.m_lod.x > 0)
			{
				uint64 frame = m_perfmon.GetFrame();

				string s;

				if(s_save && s_n >= s_saven)
				{
					s = format("c:\\temp1\\_%05d_f%lld_tex32_%05x_%d.bmp", s_n, frame, (int)m_context->TEX0.TBP0, (int)m_context->TEX0.PSM);

					t->Save(s);
				}
			}

			gd.tex[0] = t->m_buff;
			gd.sel.tw = t->m_tw - 3;

			if(m_mipmap && context->TEX1.MXL > 0 && context->TEX1.MMIN >= 2 && context->TEX1.MMIN <= 5 && m_vt.m_lod.y > 0)
			{
				// TEX1.MMIN
				// 000 p
				// 001 l
				// 010 p round
				// 011 p tri
				// 100 l round
				// 101 l tri

				if(m_vt.m_lod.x > 0)
				{
					gd.sel.ltf = context->TEX1.MMIN >> 2;
				}
				else
				{
					// TODO: isbilinear(mmag) != isbilinear(mmin) && m_vt.m_lod.x <= 0 && m_vt.m_lod.y > 0
				}

				gd.sel.mmin = (context->TEX1.MMIN & 1) + 1; // 1: round, 2: tri
				gd.sel.lcm = context->TEX1.LCM;

				int mxl = (std::min<int>((int)context->TEX1.MXL, 6) << 16);
				int k = context->TEX1.K << 12;

				if((int)m_vt.m_lod.x >= (int)context->TEX1.MXL)
				{
					k = (int)m_vt.m_lod.x << 16; // set lod to max level

					gd.sel.lcm = 1; // lod is constant
					gd.sel.mmin = 1; // tri-linear is meaningless
				}

				if(gd.sel.mmin == 2)
				{
					mxl--; // don't sample beyond the last level (TODO: add a dummy level instead?)
				}

				if(gd.sel.fst)
				{
					ASSERT(gd.sel.lcm == 1);
					ASSERT(((m_vt.m_min.t.uph(m_vt.m_max.t) == GSVector4::zero()).mask() & 3) == 3); // ratchet and clank (menu)

					gd.sel.lcm = 1;
				}

				if(gd.sel.lcm)
				{
					int lod = std::max<int>(std::min<int>(k, mxl), 0);

					if(gd.sel.mmin == 1)
					{
						lod = (lod + 0x8000) & 0xffff0000; // rounding
					}

					gd.lod.i = GSVector4i(lod >> 16);
					gd.lod.f = GSVector4i(lod & 0xffff).xxxxl().xxzz();

					// TODO: lot to optimize when lod is constant
				}
				else
				{
					gd.mxl = GSVector4((float)mxl);
					gd.l = GSVector4((float)(-0x10000 << context->TEX1.L));
					gd.k = GSVector4((float)k);
				}

				GIFRegTEX0 MIP_TEX0 = context->TEX0;
				GIFRegCLAMP MIP_CLAMP = context->CLAMP;

				GSVector4 tmin = m_vt.m_min.t;
				GSVector4 tmax = m_vt.m_max.t;

				static int s_counter = 0;

				if(0)
				//if(context->TEX0.TH > context->TEX0.TW)
				//if(s_n >= s_saven && s_n < s_saven + 3)
				//if(context->TEX0.TBP0 >= 0x2b80 && context->TEX0.TBW == 2 && context->TEX0.PSM == PSM_PSMT4)
				t->Save(format("c:/temp1/%08d_%05x_0.bmp", s_counter, context->TEX0.TBP0));

				for(int i = 1, j = std::min<int>((int)context->TEX1.MXL, 6); i <= j; i++)
				{
					switch(i)
					{
					case 1:
						MIP_TEX0.TBP0 = context->MIPTBP1.TBP1;
						MIP_TEX0.TBW = context->MIPTBP1.TBW1;
						break;
					case 2:
						MIP_TEX0.TBP0 = context->MIPTBP1.TBP2;
						MIP_TEX0.TBW = context->MIPTBP1.TBW2;
						break;
					case 3:
						MIP_TEX0.TBP0 = context->MIPTBP1.TBP3;
						MIP_TEX0.TBW = context->MIPTBP1.TBW3;
						break;
					case 4:
						MIP_TEX0.TBP0 = context->MIPTBP2.TBP4;
						MIP_TEX0.TBW = context->MIPTBP2.TBW4;
						break;
					case 5:
						MIP_TEX0.TBP0 = context->MIPTBP2.TBP5;
						MIP_TEX0.TBW = context->MIPTBP2.TBW5;
						break;
					case 6:
						MIP_TEX0.TBP0 = context->MIPTBP2.TBP6;
						MIP_TEX0.TBW = context->MIPTBP2.TBW6;
						break;
					default:
						__assume(0);
					}

					if(MIP_TEX0.TW > 0) MIP_TEX0.TW--;
					if(MIP_TEX0.TH > 0) MIP_TEX0.TH--;

					MIP_CLAMP.MINU >>= 1;
					MIP_CLAMP.MINV >>= 1;
					MIP_CLAMP.MAXU >>= 1;
					MIP_CLAMP.MAXV >>= 1;

					m_vt.m_min.t *= 0.5f;
					m_vt.m_max.t *= 0.5f;

					GSTextureCacheSW::Texture* t = m_tc->Lookup(MIP_TEX0, env.TEXA, gd.sel.tw + 3);

					if(t == NULL) {ASSERT(0); return false;}

					data2->UseSourcePages(t, i);

					GSVector4i r;

					GetTextureMinMax(r, MIP_TEX0, MIP_CLAMP, gd.sel.ltf);

					if(!t->Update(r)) {ASSERT(0); return false;}

					gd.tex[i] = t->m_buff;

					if(0)
					//if(context->TEX0.TH > context->TEX0.TW)
					//if(s_n >= s_saven && s_n < s_saven + 3)
					//if(context->TEX0.TBP0 >= 0x2b80 && context->TEX0.TBW == 2 && context->TEX0.PSM == PSM_PSMT4)
					{
						t->Save(format("c:/temp1/%08d_%05x_%d.bmp", s_counter, context->TEX0.TBP0, i));
						/*
						GIFRegTEX0 TEX0 = MIP_TEX0;
						TEX0.TBP0 = context->TEX0.TBP0;
						do
						{
							TEX0.TBP0++;
							const GSTextureCacheSW::Texture* t = m_tc->Lookup(TEX0, env.TEXA, r, gd.sel.tw + 3);
							if(t == NULL) {ASSERT(0); return false;}
							t->Save(format("c:/temp1/%08d_%05x_%d.bmp", s_counter, TEX0.TBP0, i));
						}
						while(TEX0.TBP0 < 0x3fff);
						*/

						int i = 0;
					}

				}

				s_counter++;

				m_vt.m_min.t = tmin;
				m_vt.m_max.t = tmax;
			}
			else
			{
				if(gd.sel.fst == 0)
				{
					// skip per pixel division if q is constant

					GSVertexSW* v = m_vertices;

					if(m_vt.m_eq.q)
					{
						gd.sel.fst = 1;

						if(v[0].t.z != 1.0f)
						{
							GSVector4 w = v[0].t.zzzz().rcpnr();

							for(int i = 0, j = m_count; i < j; i++)
							{
								GSVector4 t = v[i].t;

								v[i].t = (t * w).xyzw(t);
							}
						}
					}
					else if(primclass == GS_SPRITE_CLASS)
					{
						gd.sel.fst = 1;

						for(int i = 0, j = m_count; i < j; i += 2)
						{
							GSVector4 t0 = v[i + 0].t;
							GSVector4 t1 = v[i + 1].t;

							GSVector4 w = t1.zzzz().rcpnr();

							v[i + 0].t = (t0 * w).xyzw(t0);
							v[i + 1].t = (t1 * w).xyzw(t1);
						}
					}
				}

				if(gd.sel.ltf && gd.sel.fst)
				{
					// if q is constant we can do the half pel shift for bilinear sampling on the vertices

					// TODO: but not when mipmapping is used!!!

					GSVector4 half(0x8000, 0x8000);

					GSVertexSW* v = m_vertices;

					for(int i = 0, j = m_count; i < j; i++)
					{
						GSVector4 t = v[i].t;

						v[i].t = (t - half).xyzw(t);
					}
				}
			}

			uint16 tw = 1u << context->TEX0.TW;
			uint16 th = 1u << context->TEX0.TH;

			switch(context->CLAMP.WMS)
			{
			case CLAMP_REPEAT:
				gd.t.min.u16[0] = gd.t.minmax.u16[0] = tw - 1;
				gd.t.max.u16[0] = gd.t.minmax.u16[2] = 0;
				gd.t.mask.u32[0] = 0xffffffff;
				break;
			case CLAMP_CLAMP:
				gd.t.min.u16[0] = gd.t.minmax.u16[0] = 0;
				gd.t.max.u16[0] = gd.t.minmax.u16[2] = tw - 1;
				gd.t.mask.u32[0] = 0;
				break;
			case CLAMP_REGION_CLAMP:
				gd.t.min.u16[0] = gd.t.minmax.u16[0] = std::min<uint16>(context->CLAMP.MINU, tw - 1);
				gd.t.max.u16[0] = gd.t.minmax.u16[2] = std::min<uint16>(context->CLAMP.MAXU, tw - 1);
				gd.t.mask.u32[0] = 0;
				break;
			case CLAMP_REGION_REPEAT:
				gd.t.min.u16[0] = gd.t.minmax.u16[0] = context->CLAMP.MINU;
				gd.t.max.u16[0] = gd.t.minmax.u16[2] = context->CLAMP.MAXU;
				gd.t.mask.u32[0] = 0xffffffff;
				break;
			default:
				__assume(0);
			}

			switch(context->CLAMP.WMT)
			{
			case CLAMP_REPEAT:
				gd.t.min.u16[4] = gd.t.minmax.u16[1] = th - 1;
				gd.t.max.u16[4] = gd.t.minmax.u16[3] = 0;
				gd.t.mask.u32[2] = 0xffffffff;
				break;
			case CLAMP_CLAMP:
				gd.t.min.u16[4] = gd.t.minmax.u16[1] = 0;
				gd.t.max.u16[4] = gd.t.minmax.u16[3] = th - 1;
				gd.t.mask.u32[2] = 0;
				break;
			case CLAMP_REGION_CLAMP:
				gd.t.min.u16[4] = gd.t.minmax.u16[1] = std::min<uint16>(context->CLAMP.MINV, th - 1);
				gd.t.max.u16[4] = gd.t.minmax.u16[3] = std::min<uint16>(context->CLAMP.MAXV, th - 1); // ffx anima summon scene, when the anchor appears (th = 256, maxv > 256)
				gd.t.mask.u32[2] = 0;
				break;
			case CLAMP_REGION_REPEAT:
				gd.t.min.u16[4] = gd.t.minmax.u16[1] = context->CLAMP.MINV;
				gd.t.max.u16[4] = gd.t.minmax.u16[3] = context->CLAMP.MAXV;
				gd.t.mask.u32[2] = 0xffffffff;
				break;
			default:
				__assume(0);
			}

			gd.t.min = gd.t.min.xxxxlh();
			gd.t.max = gd.t.max.xxxxlh();
			gd.t.mask = gd.t.mask.xxzz();
			gd.t.invmask = ~gd.t.mask;
		}

		if(PRIM->FGE)
		{
			gd.sel.fge = 1;

			gd.frb = GSVector4i((int)env.FOGCOL.u32[0] & 0x00ff00ff);
			gd.fga = GSVector4i((int)(env.FOGCOL.u32[0] >> 8) & 0x00ff00ff);
		}

		if(context->FRAME.PSM != PSM_PSMCT24)
		{
			gd.sel.date = context->TEST.DATE;
			gd.sel.datm = context->TEST.DATM;
		}

		if(!IsOpaque())
		{
			gd.sel.abe = PRIM->ABE;
			gd.sel.ababcd = context->ALPHA.u32[0];

			if(env.PABE.PABE)
			{
				gd.sel.pabe = 1;
			}

			if(m_aa1 && PRIM->AA1 && (primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS))
			{
				gd.sel.aa1 = 1;
			}

			gd.afix = GSVector4i((int)context->ALPHA.FIX << 7).xxzzlh();
		}

		if(gd.sel.date
		|| gd.sel.aba == 1 || gd.sel.abb == 1 || gd.sel.abc == 1 || gd.sel.abd == 1
		|| gd.sel.atst != ATST_ALWAYS && gd.sel.afail == AFAIL_RGB_ONLY
		|| gd.sel.fpsm == 0 && fm != 0 && fm != 0xffffffff
		|| gd.sel.fpsm == 1 && (fm & 0x00ffffff) != 0 && (fm & 0x00ffffff) != 0x00ffffff
		|| gd.sel.fpsm == 2 && (fm & 0x80f8f8f8) != 0 && (fm & 0x80f8f8f8) != 0x80f8f8f8)
		{
			gd.sel.rfb = 1;
		}

		gd.sel.colclamp = env.COLCLAMP.CLAMP;
		gd.sel.fba = context->FBA.FBA;

		if(env.DTHE.DTHE)
		{
			gd.sel.dthe = 1;

			gd.dimx = (GSVector4i*)_aligned_malloc(sizeof(env.dimx), 32);

			memcpy(gd.dimx, env.dimx, sizeof(env.dimx));
		}
	}

	gd.sel.zwrite = zwrite;
	gd.sel.ztest = ztest;

	if(zwrite || ztest)
	{
		gd.sel.zpsm = GSLocalMemory::m_psm[context->ZBUF.PSM].fmt;
		gd.sel.ztst = ztest ? context->TEST.ZTST : ZTST_ALWAYS;
		gd.sel.zoverflow = GSVector4i(m_vt.m_max.p).z == 0x80000000;
	}

	gd.fm = GSVector4i(fm);
	gd.zm = GSVector4i(zm);

	if(gd.sel.fpsm == 1)
	{
		gd.fm |= GSVector4i::xff000000();
	}
	else if(gd.sel.fpsm == 2)
	{
		GSVector4i rb = gd.fm & 0x00f800f8;
		GSVector4i ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | GSVector4i::xffff0000();
	}

	if(gd.sel.zpsm == 1)
	{
		gd.zm |= GSVector4i::xff000000();
	}
	else if(gd.sel.zpsm == 2)
	{
		gd.zm |= GSVector4i::xffff0000();
	}

	return true;
}

template<uint32 prim, uint32 tme, uint32 fst>
void GSRendererSW::VertexKick(bool skip)
{
	const GSDrawingContext* context = m_context;

	GSVertexSW& dst = m_vl.AddTail();

	GSVector4i xy = GSVector4i::load((int)m_v.XYZ.u32[0]).upl16() - context->XYOFFSET;
	GSVector4i zf = GSVector4i((int)std::min<uint32>(m_v.XYZ.Z, 0xffffff00), m_v.FOG.F); // NOTE: larger values of z may roll over to 0 when converting back to uint32 later

	dst.p = GSVector4(xy).xyxy(GSVector4(zf) + (GSVector4::m_x4f800000 & GSVector4::cast(zf.sra32(31)))) * g_pos_scale;

	if(tme)
	{
		GSVector4 t;

		if(fst)
		{
			t = GSVector4(((GSVector4i)m_v.UV).upl16() << (16 - 4));
		}
		else
		{
			t = GSVector4(m_v.ST.S, m_v.ST.T) * GSVector4(0x10000 << context->TEX0.TW, 0x10000 << context->TEX0.TH);
			t = t.xyxy(GSVector4::load(m_v.RGBAQ.Q));
		}

		dst.t = t;
	}

	dst.c = GSVector4::rgba32(m_v.RGBAQ.u32[0], 7);

	if(prim == GS_SPRITE)
	{
		dst.t.u32[3] = m_v.XYZ.Z;
	}

	int count = 0;

	if(GSVertexSW* v = DrawingKick<prim>(skip, count))
	{
if(!m_dump)
{
		GSVector4 pmin, pmax;

		switch(prim)
		{
		case GS_POINTLIST:
			pmin = v[0].p;
			pmax = v[0].p;
			break;
		case GS_LINELIST:
		case GS_LINESTRIP:
		case GS_SPRITE:
			pmin = v[0].p.min(v[1].p);
			pmax = v[0].p.max(v[1].p);
			break;
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			pmin = v[0].p.min(v[1].p).min(v[2].p);
			pmax = v[0].p.max(v[1].p).max(v[2].p);
			break;
		}

		GSVector4 scissor = context->scissor.ex;

		GSVector4 test = (pmax < scissor) | (pmin > scissor.zwxy());

		switch(prim)
		{
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
		case GS_SPRITE:
			test |= pmin.ceil() == pmax.ceil();
			break;
		}

		switch(prim)
		{
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			// are in line or just two of them are the same (cross product == 0)
			GSVector4 tmp = (v[1].p - v[0].p) * (v[2].p - v[0].p).yxwz();
			test |= tmp == tmp.yxwz();
			break;
		}

		if(test.mask() & 3)
		{
			return;
		}
}
		switch(prim)
		{
		case GS_POINTLIST:
			break;
		case GS_LINELIST:
		case GS_LINESTRIP:
			if(PRIM->IIP == 0) {v[0].c = v[1].c;}
			break;
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			if(PRIM->IIP == 0) {v[0].c = v[2].c; v[1].c = v[2].c;}
			break;
		case GS_SPRITE:
			break;
		}

		if(m_count < 30 && m_count >= 3)
		{
			GSVertexSW* v = &m_vertices[m_count - 3];

			int tl = 0;
			int br = 0;

			bool isquad = false;

			switch(prim)
			{
			case GS_TRIANGLESTRIP:
			case GS_TRIANGLEFAN:
			case GS_TRIANGLELIST:
				isquad = GSVertexSW::IsQuad(v, tl, br);
				break;
			}

			if(isquad)
			{
				m_count -= 3;

				if(m_count > 0)
				{
					tl += m_count;
					br += m_count;

					Flush();
				}

				if(tl != 0) m_vertices[0] = m_vertices[tl];
				if(br != 1) m_vertices[1] = m_vertices[br];

				m_vertices[0].t.u32[3] = m_v.XYZ.Z;
				m_vertices[1].t.u32[3] = m_v.XYZ.Z;

				m_count = 2;

				uint32 tmp = PRIM->PRIM;
				PRIM->PRIM = GS_SPRITE;

				Flush();

				PRIM->PRIM = tmp;

				m_perfmon.Put(GSPerfMon::Quad, 1);

				return;
			}
		}

		m_count += count;

		// Flush();
	}
}

// GSRendererSW::GSRasterizerData2

GSRendererSW::GSRasterizerData2::GSRasterizerData2(GSRendererSW* parent)
	: m_parent(parent)
	, m_fb_pages(NULL)
	, m_zb_pages(NULL)
	, m_using_pages(false)
{
	memset(m_tex_pages, 0, sizeof(m_tex_pages));

	GSScanlineGlobalData* gd = (GSScanlineGlobalData*)_aligned_malloc(sizeof(GSScanlineGlobalData), 32);

	gd->sel.key = 0;

	gd->clut = NULL;
	gd->dimx = NULL;

	param = gd;
}

GSRendererSW::GSRasterizerData2::~GSRasterizerData2()
{
	if(m_using_pages)
	{
		GSScanlineGlobalData* gd = (GSScanlineGlobalData*)param;

		if(gd->sel.fwrite)
		{
			m_parent->ReleasePages(m_fb_pages, 0);
		}

		if(gd->sel.zwrite)
		{
			m_parent->ReleasePages(m_zb_pages, 1);
		}
	}

	delete m_fb_pages;
	delete m_zb_pages;

	for(size_t i = 0; i < countof(m_tex_pages) && m_tex_pages[i] != NULL; i++)
	{
		m_parent->ReleasePages(m_tex_pages[i], 2);
	}

	GSScanlineGlobalData* gd = (GSScanlineGlobalData*)param;

	if(gd->clut) _aligned_free(gd->clut);
	if(gd->dimx) _aligned_free(gd->dimx);

	_aligned_free(gd);

	m_parent->m_perfmon.Put(GSPerfMon::Fillrate, pixels);
}

void GSRendererSW::GSRasterizerData2::UseTargetPages(const vector<uint32>* fb_pages, const vector<uint32>* zb_pages)
{
	if(m_using_pages) return;

	m_fb_pages = fb_pages;
	m_zb_pages = zb_pages;

	GSScanlineGlobalData* gd = (GSScanlineGlobalData*)param;

	if(gd->sel.fwrite)
	{
		m_parent->UsePages(fb_pages, 0);
	}

	if(gd->sel.zwrite)
	{
		m_parent->UsePages(zb_pages, 1);
	}

	m_using_pages = true;
}

void GSRendererSW::GSRasterizerData2::UseSourcePages(GSTextureCacheSW::Texture* t, int level)
{
	ASSERT(m_tex_pages[level] == NULL);

	const vector<uint32>* pages = t->m_pages.n;

	m_tex_pages[level] = pages;

	m_parent->UsePages(pages, 2);
}
