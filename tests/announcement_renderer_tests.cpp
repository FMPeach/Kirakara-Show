#include "../apps/show_host/overlay/host_announcement_renderer.h"
#include "../apps/show_host/overlay/host_hud_blitter.h"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "hud_test_image.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

using kirakara::show::win32::D3D11TextureSurface;
using Microsoft::WRL::ComPtr;
void expect(bool ok,const char* text) { if(!ok){std::cerr<<"FAIL: "<<text<<'\n';std::exit(1);} }
std::vector<std::uint8_t> read(void* device,const HostHudFrame& frame) {
    D3D11TextureSurface copy;
    expect(copy.resize_on_device(device,frame.width,frame.height),"readback surface");
    ComPtr<ID3D11DeviceContext> context;
    static_cast<ID3D11Device*>(device)->GetImmediateContext(&context);
    context->CopyResource(static_cast<ID3D11Texture2D*>(copy.native_texture()),
        static_cast<ID3D11Texture2D*>(frame.texture));
    return copy.read_rgba8();
}
void save(const std::filesystem::path& path,D3D11TextureSurface& capture) {
    std::vector<BYTE> pixels(static_cast<std::size_t>(capture.width())*capture.height()*4);
    expect(capture.read_bgra8_into(pixels.data()),"capture pixels");
    for(std::size_t i=0;i<pixels.size();i+=4) if(pixels[i+3])
        for(int c=0;c<3;++c) pixels[i+c]=static_cast<BYTE>(std::min(255U,
            static_cast<unsigned>(pixels[i+c])*255/pixels[i+3]));
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    expect(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))
        &&SUCCEEDED(factory->CreateStream(&stream))
        &&SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))
        &&SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))
        &&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))
        &&SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))
        &&SUCCEEDED(frame->SetSize(capture.width(),capture.height()))&&SUCCEEDED(frame->SetPixelFormat(&format))
        &&SUCCEEDED(frame->WritePixels(capture.height(),capture.width()*4,static_cast<UINT>(pixels.size()),pixels.data()))
        &&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit()),"save PNG");
}
int main(int argc,char** argv) {
    expect(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM");
    {
        D3D11TextureSurface device;
        expect(device.resize(16,16),"device");
        auto* gpu=device.native_d3d_device();
        HostAnnouncementRenderer ticker;
        HostAnnouncementText text{1,true,L"欢迎点歌　·　正在播放：夜に駆ける　·　下一首：残酷な天使のテーゼ　·　Enjoy the music"};
        HostHudFrame card;
        card.x=1370; card.y=40;card.width=500;card.height=244;
        card.layout_ready=true;card.scene=HostHudScene::qr;
        expect(!ticker.render(gpu,1920,1080,&text,false,card,100).visible&&ticker.layouts()==0,"disabled has no layout work");
        auto first=ticker.render(gpu,1920,1080,&text,true,card,100);
        expect(first.layout_ready&&!first.visible&&first.opacity==0&&first.animating
            &&first.x==50&&first.y==40&&first.width==1280&&first.height==72,"QR avoidance geometry starts at zero opacity");
        const auto initial=read(gpu,first);
        expect(*std::max_element(initial.begin(),initial.end())<200,"intro contains only translucent background");
        const auto moving=ticker.render(gpu,1920,1080,&text,true,card,111);
        expect(read(gpu,moving)!=initial&&ticker.layouts()==1,"movement reuses text layout");
        card.opacity=0;
        const auto reserved=ticker.render(gpu,1920,1080,&text,true,card,111.1);
        expect(reserved.width==1280&&read(gpu,reserved)!=initial&&ticker.layouts()==1,
            "zero-alpha QR keeps its reserved width without restarting text");
        card.opacity=1;
        ++text.submission;
        expect(read(gpu,ticker.render(gpu,1920,1080,&text,true,card,112))==initial
            &&ticker.layouts()==1,"resending identical text restarts without shaping");
        card.scene=HostHudScene::queue;
        auto draws=ticker.rasterizations();
        text.text=L"在列表隐藏期间更新的公告"; ++text.submission;
        expect(ticker.render(gpu,1920,1080,&text,true,card,113).visible
            &&ticker.layouts()==1&&ticker.rasterizations()==draws,"hidden queue does not measure or draw");
        const auto outgoing=ticker.render(gpu,1920,1080,&text,true,card,113.075);
        expect(std::abs(outgoing.opacity-.5F)<.0001F&&outgoing.animating
            &&ticker.rasterizations()==draws,"queue handoff fades cached strip in 150ms");
        expect(!ticker.render(gpu,1920,1080,&text,true,card,113.15).visible
            &&ticker.rasterizations()==draws,"queue takeover completes without more text work");
        card={};
        auto wide=ticker.render(gpu,1920,1080,&text,true,card,114);
        expect(wide.layout_ready&&wide.opacity==0&&wide.width==1820&&ticker.layouts()==2,"hidden update fades in at full width");
        const auto wide_initial=read(gpu,wide);
        expect(read(gpu,ticker.render(gpu,1920,1080,&text,true,card,115))!=wide_initial,"idle text moves without media");
        card.target_scene=HostHudScene::queue;
        expect(ticker.render(gpu,1920,1080,&text,true,card,116).animating,"queue handoff starts strip fade-out");
        expect(!ticker.render(gpu,1920,1080,&text,true,card,116.15).visible,"strip is hidden before incoming list settles");
        card={};
        text.text=L"　 \t";++text.submission;
        draws=ticker.rasterizations();
        expect(!ticker.render(gpu,1920,1080,&text,true,card,117).visible
            &&ticker.rasterizations()==draws,"whitespace hides the whole frame");
        for (const auto size : {std::pair{640U,360U}, {960U,540U}, {720U,576U}, {1280U,720U}, {3840U,2160U}}) {
            text.text=L"m　·　あ　·　A"; ++text.submission;
            auto f=ticker.render(gpu,size.first,size.second,&text,true,{},120);
            expect(f.layout_ready&&f.x+f.width<=size.first&&f.y+f.height<=size.second,"scaled bounds");
            auto v=read(gpu,f);
            expect(v[3]==0&&v[(f.width-1)*4+3]==0,"rounded corners remain transparent");
        }
        text.text=std::wstring(5000,L'あ');++text.submission;
        expect(ticker.render(gpu,1920,1080,&text,true,{},130).visible
            &&ticker.text_width()>1920,"long text layout is not clipped to viewport width");
        text.text=L"一";++text.submission;
        expect(ticker.render(gpu,1920,1080,&text,true,{},140).visible,"single glyph");
        auto long_run=ticker.render(gpu,1920,1080,&text,true,{},140+86400.*20);
        expect(long_run.visible,"long-running short text");

        {
            HostAnnouncementRenderer single;
            const HostAnnouncementText short_text{1,true,L"TEST"};
            const auto intro=read(gpu,single.render(gpu,1920,1080,&short_text,true,{},500));
            const auto length=single.text_width();
            const auto period=(1820+length)/62;
            const auto middle=single.render(gpu,1920,1080,&short_text,true,{},500+period*.5);
            const auto pixels=read(gpu,middle);
            std::uint32_t min_x=middle.width,max_x=0;
            for(std::uint32_t y=0;y<middle.height;++y) for(std::uint32_t x=0;x<middle.width;++x) {
                const auto i=(static_cast<std::size_t>(y)*middle.width+x)*4;
                if(pixels[i]>100 || pixels[i+1]>100 || pixels[i+2]>100) {
                    min_x=std::min(min_x,x);max_x=std::max(max_x,x);
                }
            }
            expect(min_x<=max_x && min_x>=(1820-length)*.5-2
                &&max_x<=(1820+length)*.5+2,
                "short text appears only once, centered halfway through its pass");
            for(const auto offset:{period-.01,period,period+.01,2*period+.01}) {
                expect(read(gpu,single.render(gpu,1920,1080,&short_text,true,{},500+offset))==intro,
                    "cycle boundary has neither an old tail nor a prematurely repeated head");
            }
            expect(single.layouts()==1,"looping one line preserves its cached text layout");
        }

        {
            HostAnnouncementRenderer updates;
            HostAnnouncementText value{1,true,L"first"};
            for(int i=0;i<96;++i) {
                value.text=std::wstring(20+i%13,L'あ')+std::to_wstring(i);
                ++value.submission;
                const auto intro=updates.render(gpu,1920,1080,&value,true,{},i*2.);
                const auto pixels=read(gpu,intro);
                const auto layouts=updates.layouts();
                expect(layouts==static_cast<std::uint64_t>(i+1),"one layout per changed text");
                updates.render(gpu,1920,1080,&value,true,{},i*2.+.5);
                ++value.submission;
                expect(read(gpu,updates.render(gpu,1920,1080,&value,true,{},i*2.+1))==pixels
                    &&updates.layouts()==layouts,"repeated updates discard old positions and reuse equal text");
            }
            D3D11TextureSurface replacement;
            expect(replacement.resize(16,16),"replacement device");
            const auto before=updates.layouts();
            expect(updates.render(replacement.native_d3d_device(),1920,1080,&value,true,{},200).layout_ready
                &&updates.layouts()==before,"device recovery preserves device-independent layout");
        }

        {
            HostAnnouncementRenderer fade;
            HostAnnouncementText message{1,true,L"整块报幕复用浮窗淡化时间"};
            auto f=fade.render(gpu,1920,1080,&message,true,{},1000);
            expect(f.opacity==0&&!f.visible&&f.animating,"first reveal fades from transparent");
            f=fade.render(gpu,1920,1080,&message,true,{},1000.075);
            expect(std::abs(f.opacity-.5F)<.0001F,"whole strip is half visible after 75ms");
            ++message.submission;
            f=fade.render(gpu,1920,1080,&message,true,{},1000.1);
            expect(std::abs(f.opacity-2.F/3)<.0001F&&fade.layouts()==1,
                "same text resubmission does not restart component fade-in");
            f=fade.render(gpu,1920,1080,&message,true,{},1000.15);
            expect(f.opacity==1&&f.visible,"fade-in completes at the shared 150ms boundary");
            const auto pixels=read(gpu,f);
            const auto draws=fade.rasterizations();
            const auto revision=f.revision;
            fade.render(gpu,1920,1080,&message,false,{},1001);
            f=fade.render(gpu,1920,1080,&message,false,{},1001.075);
            expect(std::abs(f.opacity-.5F)<.0001F&&f.animating&&f.revision==revision
                &&fade.rasterizations()==draws&&read(gpu,f)==pixels,
                "fade-out changes only composition opacity of the cached whole strip");
            f=fade.render(gpu,1920,1080,&message,true,{},1001.1);
            expect(std::abs(f.opacity-1.F/3)<.0001F,"interrupted fade reverses from its current opacity");
            f=fade.render(gpu,1920,1080,&message,true,{},1001.25);
            expect(f.opacity==1,"re-enabled strip settles after one fade duration");
            message.text=L"　 \t";++message.submission;
            fade.render(gpu,1920,1080,&message,true,{},1002);
            const auto blank_draws=fade.rasterizations();
            f=fade.render(gpu,1920,1080,&message,true,{},1002.15);
            expect(!f.visible&&!f.animating&&fade.rasterizations()==blank_draws&&fade.layouts()==1,
                "blank text finishes fade-out without shaping or drawing");
        }

        {
            HostHudRenderer cards;
            HostAnnouncementRenderer linked;
            auto image=make_hud_test_image();
            HostHudState state;state.options.qr_enabled=state.options.queue_enabled=true;
            HostAnnouncementText message{1,true,L"报幕与二维码同步显隐"};
            HostHudPresentation phase{false,0,1,2000,true};
            const auto sample=[&](double now,bool queue) {
                phase.monotonic_seconds=now;phase.queue_visible=queue;
                const auto card=cards.render(gpu,1920,1080,&state,image,true,phase);
                return std::pair{card,linked.render(gpu,1920,1080,&message,true,card,now)};
            };
            sample(2000,false);sample(2000.15,false);
            sample(2001,true);
            auto pair=sample(2001.075,true);
            expect(std::abs(pair.first.opacity-pair.second.opacity)<.0001F,
                "QR and announcement share the outgoing 150ms fade");
            pair=sample(2001.15,true);
            expect(!pair.second.visible,"announcement is gone at the QR-to-list midpoint");
            sample(2002,false);pair=sample(2002.15,false);
            expect(pair.first.opacity==0&&pair.second.opacity==0,"both incoming components start transparent");
            pair=sample(2002.225,false);
            expect(std::abs(pair.first.opacity-.5F)<.0001F
                &&std::abs(pair.first.opacity-pair.second.opacity)<.0001F,
                "QR and announcement share the incoming 150ms fade");
            pair=sample(2002.3,false);
            expect(pair.first.opacity==1&&pair.second.opacity==1,"both components finish together");
        }

        {
            D3D11TextureSurface held,reference;
            expect(held.resize_on_device(gpu,1920,1080)
                &&reference.resize_on_device(gpu,1920,1080),"retained Cast C surfaces");
            const auto clear=[](D3D11TextureSurface& target) {
                target.begin_draw();target.clear({.2F,.3F,.4F,1});
                expect(target.end_draw(),"clean held frame");
            };
            clear(held);const auto clean=held.read_rgba8();
            HostHudRetainedBgraLayer retained;
            HostHudBlitter direct;
            text.text=L"暂停或空闲仍然滚动的公告";++text.submission;
            bool changed{};
            for(int i=0;i<15;++i) {
                HostHudLayers layers;
                layers.announcement=ticker.render(gpu,1920,1080,&text,true,{},200+i);
                expect(retained.apply(held.native_render_target(),layers,i==0,changed)&&changed,
                    "Cast C updates independent of clean media frame");
                clear(reference);
                expect(direct.draw(reference.native_render_target(),layers.announcement),"reference overlay");
                expect(held.read_rgba8()==reference.read_rgba8(),"retained ticker never compounds alpha");
            }
            expect(retained.captures()==1,"held video underlay captured once");
            expect(retained.apply(held.native_render_target(),{},false,changed)
                &&held.read_rgba8()==clean,"hiding ticker restores exact original frame");
        }

        {
            D3D11TextureSurface held,reference,red,green,blue;
            expect(held.resize_on_device(gpu,640,360)&&reference.resize_on_device(gpu,640,360)
                &&red.resize_on_device(gpu,100,30)&&green.resize_on_device(gpu,100,30)
                &&blue.resize_on_device(gpu,100,30),"overlapping HUD surfaces");
            const auto fill=[](D3D11TextureSurface& surface,kirakara::show::Color color) {
                surface.begin_draw();surface.clear(color);
                expect(surface.end_draw(),"solid overlapping tile");surface.flush_d2d();
            };
            fill(red,{1,0,0,1});fill(green,{0,1,0,1});fill(blue,{0,0,1,1});
            fill(held,{.2F,.3F,.4F,1});
            HostHudLayers layers;
            layers.announcement={blue.native_texture(),20,20,100,30,1,true};
            layers.card={red.native_texture(),30,25,50,20,1,true};
            layers.action={green.native_texture(),40,30,20,10,1,true};
            HostHudRetainedBgraLayer retained;
            HostHudBlitter ann,card,action;
            bool changed{},first=true;
            for(const auto alpha:{1.F,.5F,0.F}) {
                layers.announcement.opacity=alpha;
                layers.announcement.visible=alpha>0;
                expect(retained.apply(held.native_render_target(),layers,first,changed)&&changed,
                    "Cast C fades the announcement under an incoming card");
                first=false;fill(reference,{.2F,.3F,.4F,1});
                expect(ann.draw(reference.native_render_target(),layers.announcement)
                    &&card.draw(reference.native_render_target(),layers.card)
                    &&action.draw(reference.native_render_target(),layers.action),"reference HUD order");
                expect(held.read_rgba8()==reference.read_rgba8(),
                    "card and action remain above a fading strip without accumulated alpha");
            }
            expect(retained.captures()==3,"overlap fading reuses all three clean underlays");
        }

        if(argc>1 && std::string_view(argv[1])!="--soak-seconds") {
            std::filesystem::path output=argv[1];std::filesystem::create_directories(output);
            auto image=make_hud_test_image(); HostHudState state;
            state.options.qr_enabled=true;state.network_name=L"KiraKara_Lounge_5G";state.network_wifi=true;
            HostHudRenderer qr;HostHudBlitter card_blit,ticker_blit;
            text.text=L"欢迎点歌　·　正在播放：夜に駆ける　·　下一首：残酷な天使のテーゼ　·　当前网络 KiraKara_Lounge_5G";
            for(int theme=0;theme<3;++theme) {
                D3D11TextureSurface capture;
                expect(capture.resize_on_device(gpu,1920,1080),"visual capture");
                capture.begin_draw(); capture.clear(theme==1?kirakara::show::Color{.8F,.84F,.9F,1}:kirakara::show::Color{.035F,.05F,.08F,1});
                if(theme==2) {
                    auto* c=static_cast<ID2D1DeviceContext*>(capture.native_render_target());
                    ComPtr<ID2D1SolidColorBrush> brush;
                    c->CreateSolidColorBrush(D2D1::ColorF(.8F,.5F,.12F),&brush);
                    for(int y=0;y<1080;y+=70)for(int x=0;x<1920;x+=100)
                        if((x/100+y/70)%2)c->FillRectangle(D2D1::RectF(x,y,x+100,y+70),brush.Get());
                }
                expect(capture.end_draw(),"background draw");
                auto q=qr.render(gpu,1920,1080,&state,image,true);
                ++text.submission;ticker.render(gpu,1920,1080,&text,true,q,100);
                auto a=ticker.render(gpu,1920,1080,&text,true,q,128);
                expect(card_blit.draw(capture.native_render_target(),q)&&ticker_blit.draw(capture.native_render_target(),a),"visual composite");
                save(output/(std::to_string(theme)+"-announcement.png"),capture);
            }
        }

        text.text=L"固定公告　·　纯文本滚动　·　Continuous time";++text.submission;
        const auto begin=std::chrono::steady_clock::now();
        const bool soak=argc>2&&std::string_view(argv[1])=="--soak-seconds";
        const auto seconds=soak?std::max(1,std::atoi(argv[2])):0;
        PROCESS_MEMORY_COUNTERS start{},end{}; start.cb=end.cb=sizeof(start);
        GetProcessMemoryInfo(GetCurrentProcess(),&start,sizeof(start));
        DWORD h0{},h1{};GetProcessHandleCount(GetCurrentProcess(),&h0);
        auto layout_start=ticker.layouts();
        std::vector<double> durations;
        for(int i=0;soak || i<600;++i) {
            auto t=std::chrono::steady_clock::now();
            if(soak&&t-begin>=std::chrono::seconds(seconds))break;
            const double now=soak?std::chrono::duration<double>(t-begin).count():i/60.;
            expect(ticker.render(gpu,1920,1080,&text,true,{},now).visible,"steady benchmark");
            durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count());
            if(soak)std::this_thread::sleep_until(begin+std::chrono::microseconds((i+1)*16667LL));
        }
        GetProcessMemoryInfo(GetCurrentProcess(),&end,sizeof(end));GetProcessHandleCount(GetCurrentProcess(),&h1);
        std::sort(durations.begin(),durations.end());
        expect(ticker.layouts()==layout_start+1,"steady draw shapes text just once");
        std::cout<<"ANNOUNCEMENT frames="<<durations.size()<<" p95_ms="<<durations[durations.size()*95/100]
            <<" max_ms="<<durations.back()<<" layouts="<<ticker.layouts()-layout_start
            <<" working_set_delta="<<static_cast<long long>(end.WorkingSetSize)-static_cast<long long>(start.WorkingSetSize)
            <<" handles_delta="<<static_cast<long long>(h1)-h0<<'\n';
    }
    CoUninitialize();std::cout<<"Announcement renderer tests passed.\n";
}
