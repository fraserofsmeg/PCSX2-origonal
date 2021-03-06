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

#pragma once

#include "GSRenderer.h"
#include "GSTextureCacheSW.h"
#include "GSDrawScanline.h"

class GSRendererSW : public GSRendererT<GSVertexSW>
{
	class GSRasterizerData2 : public GSRasterizerData
	{
		GSRendererSW* m_parent;
		const vector<uint32>* m_fb_pages;
		const vector<uint32>* m_zb_pages;
		const vector<uint32>* m_tex_pages[7];
		bool m_using_pages;

	public:
		GSRasterizerData2(GSRendererSW* parent);
		virtual ~GSRasterizerData2();

		void UseTargetPages(const vector<uint32>* fb_pages, const vector<uint32>* zb_pages);
		void UseSourcePages(GSTextureCacheSW::Texture* t, int level);
	};

protected:
	IRasterizer* m_rl;
	GSTextureCacheSW* m_tc;
	GSTexture* m_texture[2];
	uint8* m_output;
	bool m_reset;
	GSPixelOffset4* m_fzb;
	uint32 m_fzb_pages[512]; // uint16 frame/zbuf pages interleaved
	uint16 m_tex_pages[512];

	void Reset();
	void VSync(int field);
	void ResetDevice();
	GSTexture* GetOutput(int i);

	void Draw();
	void Sync(int reason);
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r);
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false);

	void UsePages(const vector<uint32>* pages, int type);
	void ReleasePages(const vector<uint32>* pages, int type);

	bool GetScanlineGlobalData(GSRasterizerData2* data2);

public:
	GSRendererSW(int threads);
	virtual ~GSRendererSW();

	template<uint32 prim, uint32 tme, uint32 fst>
	void VertexKick(bool skip);
};
