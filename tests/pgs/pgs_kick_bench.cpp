#include "common.h"
#include "pgs_vertex_kernels.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <ctime>
#include <algorithm>

static Regs R; static int OFX, OFY;

static inline pgs_kick_regs gather()
{
	pgs_kick_regs g;
	g.st = R.st.bits; g.rgbaq = R.rgbaq.bits;
	g.uv = R.uv.words[0]; g.fog = R.fog.words[1] >> 24;
	g.ofx = OFX; g.ofy = OFY;
	return g;
}

/* ---- Q0: current, muglm, shift queue ---- */
struct Q0 {
	VertexPosition pos[3]; VertexAttribute attr[3]; unsigned count = 0;
	inline void shift(){ if(count==3){pos[0]=pos[1];attr[0]=attr[1];pos[1]=pos[2];attr[1]=attr[2];count=2;} }
	inline void kick(uint64_t v, bool f){
		shift();
		auto &p = pos[count]; auto &a = attr[count];
		if (f) { Reg64<XYZFBits> x(v);
			p.pos.x = int(x.desc.X)-OFX; p.pos.y = int(x.desc.Y)-OFY; p.z = x.desc.Z;
			a.st.x=R.st.desc.S; a.st.y=R.st.desc.T; a.q=R.rgbaq.desc.Q; a.rgba=R.rgbaq.words[0];
			a.fog=float(x.desc.F); a.uv=u16vec2(R.uv.desc.U,R.uv.desc.V);
		} else { Reg64<XYZBits> x(v);
			p.pos.x = int(x.desc.X)-OFX; p.pos.y = int(x.desc.Y)-OFY; p.z = x.desc.Z;
			a.st.x=R.st.desc.S; a.st.y=R.st.desc.T; a.q=R.rgbaq.desc.Q; a.rgba=R.rgbaq.words[0];
			a.fog=float(R.fog.desc.FOG); a.uv=u16vec2(R.uv.desc.U,R.uv.desc.V);
		}
		count++;
	}
	inline unsigned slot(unsigned i) const { return i; }
	inline void m_list(){ count=0; }
};

/* ---- QC: C89 kernels, shift queue (queue untouched) ---- */
struct QC {
	VertexPosition pos[3]; VertexAttribute attr[3]; unsigned count = 0;
	inline void shift(){ if(count==3){pos[0]=pos[1];attr[0]=attr[1];pos[1]=pos[2];attr[1]=attr[2];count=2;} }
	inline void kick(uint64_t v, bool f){
		pgs_kick_regs g = gather();
		uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
		shift();
		pgs_build_position(&g, lo, f ? (hi & 0xffffffu) : hi, &pos[count]);
		pgs_build_attribute(&g, f ? (float)(hi >> 24) : (float)g.fog, &attr[count]);
		count++;
	}
	inline unsigned slot(unsigned i) const { return i; }
	inline void m_list(){ count=0; }
};

/* ---- QCR: C89 kernels + ring queue ---- */
struct QCR {
	alignas(16) VertexPosition pos[4]; alignas(16) VertexAttribute attr[4];
	unsigned count = 0, head = 0;
	inline unsigned slot(unsigned i) const { return (head+i)&3; }
	inline void kick(uint64_t v, bool f){
		pgs_kick_regs g = gather();
		uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
		unsigned s;
		if (count==3){ head=(head+1)&3; count=2; }
		s = slot(count);
		pgs_build_position(&g, lo, f ? (hi & 0xffffffu) : hi, &pos[s]);
		pgs_build_attribute(&g, f ? (float)(hi >> 24) : (float)g.fog, &attr[s]);
		count++;
	}
	inline void m_list(){ count=0; head=0; }
};

/* ---- QCRP: C89 kernels + ring queue + padded position store ---- */
struct QCRP {
	alignas(16) VertexPosition pos[4]; alignas(16) VertexAttribute attr[4];
	unsigned count = 0, head = 0;
	inline unsigned slot(unsigned i) const { return (head+i)&3; }
	inline void kick(uint64_t v, bool f){
		pgs_kick_regs g = gather();
		uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
		unsigned s;
		if (count==3){ head=(head+1)&3; count=2; }
		s = slot(count);
		pgs_build_position_padded(&g, lo, f ? (hi & 0xffffffu) : hi, &pos[s]);
		pgs_build_attribute(&g, f ? (float)(hi >> 24) : (float)g.fog, &attr[s]);
		count++;
	}
	inline void m_list(){ count=0; head=0; }
};

struct Rec { uint64_t st, rgbaq, uv, fog, xyz; };
static std::vector<Rec> mk(size_t n){
	std::vector<Rec> v(n); uint64_t s=0x9E3779B97F4A7C15ull;
	auto nx=[&]{ s=s*6364136223846793005ull+1442695040888963407ull; return s>>16; };
	for(size_t i=0;i<n;i++){v[i].st=nx();v[i].rgbaq=nx();v[i].uv=nx();v[i].fog=nx();v[i].xyz=nx();}
	return v;
}
template <typename Q>
static inline uint64_t consume(const Q &q){
	uint64_t s=0;
	for(unsigned k=0;k<3;k++){
		const auto &p=q.pos[q.slot(2-k)]; const auto &a=q.attr[q.slot(2-k)];
		s += uint64_t(uint32_t(p.pos.x))+uint64_t(uint32_t(p.pos.y))+p.z;
		uint32_t w[6]; memcpy(w,&a,24); for(int j=0;j<6;j++) s+=w[j];
	}
	return s;
}
template <typename Q, bool XYZF, bool LIST>
static uint64_t run(const std::vector<Rec> &w, int iters){
	Q q; uint64_t sink=0;
	for(int it=0;it<iters;it++){ q.m_list();
		for(size_t i=0;i<w.size();i++){
			R.st.bits=w[i].st; R.rgbaq.bits=w[i].rgbaq; R.uv.bits=w[i].uv; R.fog.bits=w[i].fog;
			q.kick(w[i].xyz, XYZF);
			if(q.count>=3){ sink+=consume(q); if(LIST) q.m_list(); }
		}
	}
	return sink;
}
static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+1e-9*t.tv_nsec; }
typedef uint64_t (*Fn)(const std::vector<Rec>&,int);
struct V { const char*n; Fn f; std::vector<double> s; };
static uint64_t SINK;
static void grp(const char*title, std::vector<V>&vs, const std::vector<Rec>&w,int it,int rounds){
	for(auto&v:vs){v.s.clear(); SINK+=v.f(w,2);}
	for(int r=0;r<rounds;r++) for(auto&v:vs){ double a=now(); SINK+=v.f(w,it); double b=now();
		v.s.push_back((b-a)*1e9/(double(w.size())*it)); }
	printf("\n%s  (%d rounds, median ns/vertex)\n", title, rounds);
	double base=0;
	for(auto&v:vs){ std::sort(v.s.begin(),v.s.end()); double m=v.s[v.s.size()/2];
		if(base==0) base=m;
		printf("  %-26s %7.3f   %+6.1f%%\n", v.n, m, 100.0*(m-base)/base); }
}
int main(int argc,char**argv){
	int it = argc>1?atoi(argv[1]):2500, rounds = argc>2?atoi(argv[2]):25;
	auto w = mk(4096); OFX=1024<<PGS_SUBPIXEL_BITS; OFY=OFX;
	std::vector<V> a={{"muglm (current)",run<Q0,false,false>,{}},
	                  {"C89 kernels",run<QC,false,false>,{}},
	                  {"C89 + ring queue",run<QCR,false,false>,{}},
	                  {"C89 + ring + pad store",run<QCRP,false,false>,{}}};
	std::vector<V> b={{"muglm (current)",run<Q0,true,false>,{}},
	                  {"C89 kernels",run<QC,true,false>,{}},
	                  {"C89 + ring queue",run<QCR,true,false>,{}},
	                  {"C89 + ring + pad store",run<QCRP,true,false>,{}}};
	std::vector<V> c={{"muglm (current)",run<Q0,false,true>,{}},
	                  {"C89 kernels",run<QC,false,true>,{}},
	                  {"C89 + ring queue",run<QCR,false,true>,{}},
	                  {"C89 + ring + pad store",run<QCRP,false,true>,{}}};
	grp("triangle strip, XYZ2",a,w,it,rounds);
	grp("triangle strip, XYZF2",b,w,it,rounds);
	grp("triangle list / sprite, XYZ2",c,w,it,rounds);
	if(SINK==0x1234567ull) printf("x\n");
	return 0;
}
