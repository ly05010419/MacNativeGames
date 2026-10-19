// Synthetic DSP reference vectors, no game assets or device audio.
// Reverb uses Room: the local reference ignores other EarlyMode table rows.
// Other early modes have separate impulse checks in mixer_probe.c (Ryujinx.Audio's tables, indexed by mode).
using System;
using System.IO;
using System.Runtime.InteropServices;
using Ryujinx.Audio.Renderer.Common;
using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Audio.Renderer.Parameter.Effect;
using Ryujinx.Audio.Renderer.Dsp.Command;
using Ryujinx.Audio.Renderer.Dsp.State;
using Ryujinx.Audio.Renderer.Server.Effect;

class Program {
    const int N=240, Blocks=100;
    static int Q(float x)=>(int)MathF.Round(x*16384);
    static void Param<T>(BinaryWriter w,T p) where T:unmanaged {
        var data=MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref p,1));
        w.Write(data);w.Write(new byte[160-data.Length]);
    }
    static void Main(string[] args) {
        using var writer=new BinaryWriter(File.Create(args[0]));
        writer.Write(0x41454658);writer.Write(24);
        foreach(int type in new[]{3,4,9})foreach(ushort channels in new ushort[]{1,2,4,6})foreach(bool mapping in new[]{false,true}) {
            writer.Write(type);writer.Write(mapping?11:10);writer.Write((int)channels);writer.Write(N);writer.Write(Blocks);
            var delay=new DelayParameter{ChannelCount=channels,ChannelCountMax=6,DelayTimeMax=40,DelayTime=5,SampleRate=(uint)Q(48000),InGain=(uint)Q(.8f),FeedbackGain=(uint)Q(.4f),OutGain=(uint)Q(.6f),DryGain=(uint)Q(.25f),ChannelSpread=(uint)Q(.35f),LowPassAmount=(uint)Q(.3f)};
            var reverb=new ReverbParameter{ChannelCount=channels,ChannelCountMax=6,SampleRate=Q(48),EarlyMode=(ReverbEarlyMode)0,LateMode=(ReverbLateMode)1,EarlyGain=Q(.4f),PreDelayTime=Q(12),LateGain=Q(.5f),DecayTime=Q(1100),HighFrequencyDecayRatio=Q(.8f),Coloration=Q(.3f),ReverbGain=Q(.3f),OutGain=Q(.7f),DryGain=Q(.2f)};
            var compressor=new CompressorParameter{ChannelCount=channels,ChannelCountMax=6,SampleRate=48000,Threshold=-18,Ratio=4,InputGain=.05f,AttackCoefficient=.2f,ReleaseCoefficient=.02f,OutputGain=-1,MakeupGainEnabled=true,StatisticsEnabled=true};
            for(int c=0;c<channels;c++){delay.Input[c]=reverb.Input[c]=compressor.Input[c]=(byte)c;delay.Output[c]=reverb.Output[c]=compressor.Output[c]=(byte)(c+6);}
            var ds=new DelayState[1];var rs=new ReverbState[1];var cs=new CompressorState[1];var stats=new EffectResultState[1];
            ref var stat=ref MemoryMarshal.Cast<byte,CompressorStatistics>(stats[0].SpecificData)[0];stat.Reset(channels);
            var buffer=new float[12*N];using var ctx=new CommandList(null,buffer,N,48000,12,0);
            for(int b=0;b<Blocks;b++) {
                bool enabled=b<50||b>=60,reset=b==0||b==70;
                UsageState status=reset?UsageState.Invalid:b==30?UsageState.New:UsageState.Enabled;
                delay.Status=reverb.Status=compressor.Status=status;
                if(b==30){delay.FeedbackGain=(uint)Q(.2f);delay.LowPassAmount=(uint)Q(.5f);reverb.Coloration=Q(.6f);reverb.LateMode=(ReverbLateMode)3;compressor.Threshold=-8;compressor.Ratio=2;}
                compressor.StatisticsReset=b==25;
                if(reset)stat.Reset(channels);
                writer.Write(enabled?1:0);writer.Write(reset?1:0);
                if(type==3)Param(writer,delay);else if(type==4)Param(writer,reverb);else Param(writer,compressor);
                Array.Clear(buffer);
                for(int c=0;c<channels;c++)for(int k=0;k<N;k++) {
                    int t=b*N+k;float x=t<16000?(((t*17+c*31)%257)-128)/512f:0;
                    if(t%701==c*73)x=.65f;
                    buffer[c*N+k]=x*32768;writer.Write(x);
                }
                if(type==3)new DelayCommand().Initialize(0,delay,ds,enabled,0,0,mapping).Process(ctx);
                else if(type==4)new ReverbCommand().Initialize(0,reverb,rs,enabled,0,0,true,mapping).Process(ctx);
                else new CompressorCommand().Initialize(0,compressor,cs,stats,enabled,0).Process(ctx);
                for(int c=0;c<channels;c++)for(int k=0;k<N;k++)writer.Write(buffer[(c+6)*N+k]/32768);
                if(type==9){writer.Write(stat.MaximumMean);writer.Write(stat.MinimumGain);for(int c=0;c<6;c++)writer.Write(stat.LastSamples[c]);}
            }
        }
        Console.WriteLine("Wrote 24 cases including legacy and REV11 channel mapping");
    }
}
