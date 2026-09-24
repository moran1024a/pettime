using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

static class Program
{
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(uint thread);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr h,int index,System.Text.StringBuilder value,int length,out int needed);
    static string DesktopKey(){var value=new System.Text.StringBuilder(256);int needed;if(!GetUserObjectInformation(GetThreadDesktop(GetCurrentThreadId()),2,value,512,out needed))throw new System.ComponentModel.Win32Exception();return value.ToString().Replace('\\','_');}
    public static int ReadAffinity(){try{return Math.Max(0,Math.Min(100,int.Parse(File.ReadAllText(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"XiaoqiangPet","affinity.dat")))));}catch{return 0;}}
    public static void SaveAffinity(int value){string dir=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"XiaoqiangPet");Directory.CreateDirectory(dir);string path=Path.Combine(dir,"affinity.dat"),tmp=path+".tmp";File.WriteAllText(tmp,Math.Max(0,Math.Min(100,value)).ToString());if(File.Exists(path))File.Replace(tmp,path,null);else File.Move(tmp,path);}
    public static void Log(string message) { try { File.AppendAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"runtime.log"),DateTime.Now.ToString("O")+" "+message+Environment.NewLine); } catch {} }
    static void Fail(Exception ex) { Log(ex.ToString()); MessageBox.Show(ex.Message+"\n详情见程序目录 runtime.log", "小强桌宠运行失败",MessageBoxButtons.OK,MessageBoxIcon.Error); Environment.ExitCode=1; }
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [STAThread] static void Main(string[] args)
    {
        try
        {
            SetProcessDPIAware();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            Application.ThreadException += delegate(object s,ThreadExceptionEventArgs e){Fail(e.Exception);Application.Exit();};
            string root = AppDomain.CurrentDomain.BaseDirectory;
            string desktop=DesktopKey();Log("Launch desktop="+desktop);
            using(EventWaitHandle recall = new EventWaitHandle(false,EventResetMode.AutoReset,"Local\\XiaoqiangPetV5Recall_"+desktop))
            {
                bool first;
                using (Mutex mutex = new Mutex(true, "Local\\XiaoqiangPetV5_"+desktop, out first))
                {
                    if (!first) { recall.Set(); Log("Existing instance recalled by launch."); return; }
                    using(Rig rig=new Rig())
                    {
                        if(args.Length>0 && args[0]=="--export"){rig.Export(Path.Combine(root,"preview"));return;}
                        Log("Starting PID "+Process.GetCurrentProcess().Id);
                        Application.Run(new Pet(rig,args.Length>0 && args[0]=="--smoke",recall));
                        Log("Stopped.");
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Fail(ex);
        }
    }
}

// 48 samples of a distance-driven alternating tripod gait. All six limbs articulate.
sealed class Rig : IDisposable
{
    public const int FrontLaunchFrames=14,FrontFlightFrames=32,FrontDiveFrames=16,FrontRecoilFrames=10;
    public const int FrontFlightStart=FrontLaunchFrames,FrontDiveStart=FrontFlightStart+FrontFlightFrames,FrontRecoilStart=FrontDiveStart+FrontDiveFrames,FrontPounceFrameCount=FrontRecoilStart+FrontRecoilFrames;
    public readonly Bitmap[] Walk = new Bitmap[72], Idle = new Bitmap[48];
    public readonly Bitmap[] Pounce = new Bitmap[24], FrontPounce = new Bitmap[FrontPounceFrameCount], NymphPounce = new Bitmap[24];
    public readonly Bitmap[] Crush = new Bitmap[16], NymphCrush = new Bitmap[16], Nymph = new Bitmap[48], NymphIdle = new Bitmap[12];
    readonly Bitmap body;
    readonly Rectangle crop;
    readonly Bitmap nymphBody;
    readonly Rectangle nymphCrop;
    const float TAU = (float)(Math.PI * 2);
    public Rig()
    {
        using(Stream source=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachBody"))
        {
            if(source==null)throw new InvalidDataException("程序缺少内嵌素材，请重新构建。");
            using(Bitmap loaded=new Bitmap(source))body=new Bitmap(loaded);
        }
        crop=AlphaBounds(body,32,0);
        for(int i=0;i<Walk.Length;i++) Walk[i]=Frame(i/(float)Walk.Length,true);
        for(int i=0;i<Idle.Length;i++) Idle[i]=Frame(i/(float)Idle.Length,false);
        using(Stream source=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachNymph"))
        {
            if(source==null)throw new InvalidDataException("程序缺少若虫身体素材，请重新构建。");
            using(Bitmap loaded=new Bitmap(source))nymphBody=new Bitmap(loaded);
        }
        nymphCrop=AlphaBounds(nymphBody,24,0);
        for(int i=0;i<Nymph.Length;i++)Nymph[i]=NymphFrame(i/(float)Nymph.Length,true);
        for(int i=0;i<NymphIdle.Length;i++)NymphIdle[i]=NymphFrame(i/(float)NymphIdle.Length,false);
        using(Stream source=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachPounce"))
        {
            if(source==null)throw new InvalidDataException("程序缺少飞扑动作素材，请重新构建。");
            using(Bitmap atlas=new Bitmap(source))
            {
                for(int i=0;i<Pounce.Length;i++)
                {
                    using(Bitmap raw=CloneAtlasCell(atlas,i%6,i/6,6,4,24)) Pounce[i]=Normalize(raw);
                }
            }
        }
        using(Stream baseSource=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachFrontPounceBase"))
        using(Stream upSource=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachFrontPounceUp"))
        using(Stream downSource=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachFrontPounceDown"))
        using(Stream crouchSource=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachFrontPounceCrouch"))
        using(Stream transitionSource=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachFrontPounceTransition"))
        {
            if(baseSource==null||upSource==null||downSource==null||crouchSource==null||transitionSource==null)throw new InvalidDataException("程序缺少正面飞扑动作素材，请重新构建。");
            using(Bitmap rawBase=new Bitmap(baseSource))using(Bitmap rawUp=new Bitmap(upSource))using(Bitmap rawDown=new Bitmap(downSource))using(Bitmap rawCrouch=new Bitmap(crouchSource))using(Bitmap rawTransition=new Bitmap(transitionSource))
            {
                Rectangle referenceBounds=AlphaBounds(rawBase,24,1);
                using(Bitmap basePose=NormalizeFrontPose(rawBase,referenceBounds))
                using(Bitmap upPose=NormalizeFrontPose(rawUp,referenceBounds))
                using(Bitmap downPose=NormalizeFrontPose(rawDown,referenceBounds))
                using(Bitmap crouchPose=NormalizeFrontPose(rawCrouch,referenceBounds))
                using(Bitmap transitionPose=NormalizeFrontPose(rawTransition,referenceBounds))
                    for(int i=0;i<FrontPounce.Length;i++)FrontPounce[i]=CreateFrontPounceFrame(basePose,upPose,downPose,crouchPose,transitionPose,Walk,i);
            }
        }
        using(Stream source=System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("RoachCrush"))
        {
            if(source==null)throw new InvalidDataException("程序缺少碾碎动作素材，请重新构建。");
            using(Bitmap atlas=new Bitmap(source))for(int i=0;i<Crush.Length;i++)
            {
                using(Bitmap raw=CloneAtlasCell(atlas,i%4,i/4,4,4,32))Crush[i]=Normalize(raw);
            }
        }
        Bitmap[] matchedAdultCrush=MatchPoseSize(Crush,Walk,Idle,512);
        Bitmap[] matchedNymphCrush=MatchPoseSize(Crush,Nymph,NymphIdle,256);
        Bitmap[] matchedNymphPounce=MatchPoseSize(Pounce,Nymph,NymphIdle,256);
        for(int i=0;i<Crush.Length;i++){Crush[i].Dispose();Crush[i]=matchedAdultCrush[i];NymphCrush[i]=matchedNymphCrush[i];}
        for(int i=0;i<NymphPounce.Length;i++)NymphPounce[i]=matchedNymphPounce[i];
    }
    static Bitmap[] MatchPoseSize(Bitmap[] poses,Bitmap[] referenceA,Bitmap[] referenceB,int canvasSize)
    {
        long maxArea=1;
        int stepA=Math.Max(1,referenceA.Length/4),stepB=Math.Max(1,referenceB.Length/4);
        for(int i=0;i<referenceA.Length;i+=stepA){Rectangle r=AlphaBounds(referenceA[i]);maxArea=Math.Max(maxArea,(long)r.Width*r.Height);}
        for(int i=0;i<referenceB.Length;i+=stepB){Rectangle r=AlphaBounds(referenceB[i]);maxArea=Math.Max(maxArea,(long)r.Width*r.Height);}
        if(referenceA.Length>0){Rectangle r=AlphaBounds(referenceA[referenceA.Length-1]);maxArea=Math.Max(maxArea,(long)r.Width*r.Height);}
        if(referenceB.Length>0){Rectangle r=AlphaBounds(referenceB[referenceB.Length-1]);maxArea=Math.Max(maxArea,(long)r.Width*r.Height);}
        float diameter=(float)Math.Sqrt(maxArea);Bitmap[] result=new Bitmap[poses.Length];
        for(int i=0;i<poses.Length;i++)
        {
            float size=diameter;Bitmap fitted=new Bitmap(canvasSize,canvasSize,PixelFormat.Format32bppPArgb);
            using(Graphics g=Graphics.FromImage(fitted)){g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(poses[i],new RectangleF((canvasSize-size)/2,(canvasSize-size)/2,size,size),new Rectangle(0,0,poses[i].Width,poses[i].Height),GraphicsUnit.Pixel);}
            result[i]=fitted;
        }
        return result;
    }
    static Bitmap Normalize(Bitmap raw)
    {
        RemoveBackgroundIslands(raw);
        Rectangle bounds=AlphaBounds(raw);float scale=Math.Min(1.84f,Math.Min(476f/bounds.Width,476f/bounds.Height));
        float w=bounds.Width*scale,h=bounds.Height*scale;
        Bitmap pose=new Bitmap(512,512,PixelFormat.Format32bppPArgb);
        using(Graphics g=Graphics.FromImage(pose)){g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(raw,new RectangleF((512-w)/2,(512-h)/2,w,h),bounds,GraphicsUnit.Pixel);}
        return pose;
    }
    static Bitmap CloneAtlasCell(Bitmap atlas,int column,int row,int columns,int rows,int bleed)
    {
        int left=column*atlas.Width/columns,right=(column+1)*atlas.Width/columns,top=row*atlas.Height/rows,bottom=(row+1)*atlas.Height/rows;
        Rectangle cell=Rectangle.FromLTRB(Math.Max(0,left-bleed),Math.Max(0,top-bleed),Math.Min(atlas.Width,right+bleed),Math.Min(atlas.Height,bottom+bleed));
        return atlas.Clone(cell,PixelFormat.Format32bppPArgb);
    }
    static void RemoveBackgroundIslands(Bitmap image)
    {
        int width=image.Width,height=image.Height,count=width*height;
        BitmapData data=image.LockBits(new Rectangle(0,0,width,height),ImageLockMode.ReadWrite,PixelFormat.Format32bppPArgb);
        try
        {
            int stride=Math.Abs(data.Stride);byte[] pixels=new byte[stride*height];Marshal.Copy(data.Scan0,pixels,0,pixels.Length);
            int[] labels=new int[count],queue=new int[count];int nextLabel=0,bestLabel=0,bestSize=0;
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)
            {
                int index=y*width+x,offset=y*stride+x*4+3;if(labels[index]!=0||pixels[offset]<=24)continue;
                int label=++nextLabel,head=0,tail=0;queue[tail++]=index;labels[index]=label;
                while(head<tail)
                {
                    int p=queue[head++],px=p%width,py=p/width;
                    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)
                    {
                        int xx=px+dx,yy=py+dy;if(xx<0||xx>=width||yy<0||yy>=height)continue;
                        int q=yy*width+xx;if(labels[q]==0&&pixels[yy*stride+xx*4+3]>24){labels[q]=label;queue[tail++]=q;}
                    }
                }
                if(tail>bestSize){bestSize=tail;bestLabel=label;}
            }
            bool[] keep=new bool[count];
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)if(labels[y*width+x]==bestLabel)
                for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){int xx=x+dx,yy=y+dy;if(xx>=0&&xx<width&&yy>=0&&yy<height)keep[yy*width+xx]=true;}
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)if(!keep[y*width+x]){int o=y*stride+x*4;pixels[o]=pixels[o+1]=pixels[o+2]=pixels[o+3]=0;}
            Marshal.Copy(pixels,0,data.Scan0,pixels.Length);
        }
        finally{image.UnlockBits(data);}
    }
    static Rectangle AlphaBounds(Bitmap image){return AlphaBounds(image,24,3);}
    static Rectangle AlphaBounds(Bitmap image,int threshold,int padding)
    {
        int x0=image.Width,y0=image.Height,x1=-1,y1=-1,width=image.Width,height=image.Height,rowLength=image.Width*4;
        BitmapData data=image.LockBits(new Rectangle(0,0,width,height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
        try
        {
            byte[] pixels=new byte[rowLength*height];
            for(int y=0;y<height;y++)Marshal.Copy(IntPtr.Add(data.Scan0,y*data.Stride),pixels,y*rowLength,rowLength);
            for(int y=0;y<height;y++)for(int x=0;x<width;x++)if(pixels[y*rowLength+x*4+3]>threshold){x0=Math.Min(x0,x);y0=Math.Min(y0,y);x1=Math.Max(x1,x);y1=Math.Max(y1,y);}
        }
        finally{image.UnlockBits(data);}
        if(x1<x0||y1<y0)throw new InvalidDataException("透明帧缺少有效轮廓。");
        return Rectangle.FromLTRB(Math.Max(0,x0-padding),Math.Max(0,y0-padding),Math.Min(width,x1+1+padding),Math.Min(height,y1+1+padding));
    }
    static PointF P(float x,float y) {return new PointF(x,y);}
    static void Segment(Graphics g,PointF a,PointF b,float width)
    {
        float dx=b.X-a.X,dy=b.Y-a.Y,length=(float)Math.Sqrt(dx*dx+dy*dy);if(length<.1f)return;
        float nx=-dy/length,ny=dx/length,h=width*.55f,tip=width*.16f,inner=width*.28f;
        PointF[] shell={P(a.X+nx*h,a.Y+ny*h),P(b.X+nx*tip,b.Y+ny*tip),P(b.X-nx*tip,b.Y-ny*tip),P(a.X-nx*h,a.Y-ny*h)};
        PointF[] chitin={P(a.X+dx*.13f+nx*inner,a.Y+dy*.13f+ny*inner),P(b.X-dx*.07f+nx*tip*.58f,b.Y-dy*.07f+ny*tip*.58f),P(b.X-dx*.07f-nx*tip*.58f,b.Y-dy*.07f-ny*tip*.58f),P(a.X+dx*.13f-nx*inner,a.Y+dy*.13f-ny*inner)};
        using(SolidBrush dark=new SolidBrush(Color.FromArgb(255,39,22,14)))g.FillPolygon(dark,shell);
        using(SolidBrush mid=new SolidBrush(Color.FromArgb(255,111,58,28)))g.FillPolygon(mid,chitin);
        using(Pen edge=new Pen(Color.FromArgb(240,48,26,15),.48f))g.DrawPolygon(edge,shell);
        using(Pen ridge=new Pen(Color.FromArgb(165,193,131,75),Math.Max(.35f,width*.12f)))g.DrawLine(ridge,P(a.X+dx*.1f-nx*inner*.22f,a.Y+dy*.1f-ny*inner*.22f),P(b.X-dx*.11f-nx*tip*.38f,b.Y-dy*.11f-ny*tip*.38f));
        float joint=width*.34f;using(SolidBrush knuckle=new SolidBrush(Color.FromArgb(255,76,39,22)))g.FillEllipse(knuckle,a.X-joint,a.Y-joint,joint*2,joint*2);
    }
    Bitmap Frame(float phase,bool walking)
    {
        Bitmap result=new Bitmap(512,512,PixelFormat.Format32bppPArgb);
        using(Graphics g=Graphics.FromImage(result))
        {
            g.Clear(Color.Transparent);g.SmoothingMode=SmoothingMode.AntiAlias;g.InterpolationMode=InterpolationMode.HighQualityBicubic;
            g.ScaleTransform(2,2);g.TranslateTransform(128,141);
            float motion=walking?1:0;
            for(int side=-1;side<=1;side+=2) for(int leg=0;leg<3;leg++)
            {
                bool firstTripod=(leg%2==0)==(side==1);
                float cycle=(phase+(firstTripod?0:.5f))%1f;
                const float stance=.64f;
                float stride,lift=0;
                if(cycle<stance) stride=-12+24*(cycle/stance);
                else {float swing=(cycle-stance)/(1-stance);float eased=swing*swing*(3-2*swing);stride=12-24*eased;lift=(float)Math.Sin(swing*Math.PI)*7;}
                stride*=motion;lift*=motion;
                float hipY=-29+leg*19;
                float kneeY=hipY+new float[]{-16,1,18}[leg]+stride*.22f-lift*.18f;
                float footY=hipY+new float[]{-27,7,39}[leg]+stride;
                float footX=side*(52-lift*.65f);
                PointF hip=P(side*15,hipY), knee=P(side*(31+lift*.35f),kneeY), ankle=P(footX,footY);
                PointF toe=P(side*(59-lift*.45f),footY+3+stride*.1f);
                Segment(g,hip,knee,3.1f);Segment(g,knee,ankle,1.55f);Segment(g,ankle,toe,.72f);
                using(Pen spine=new Pen(Color.FromArgb(225,67,36,19),.48f))
                for(int n=1;n<6;n++) {float f=n/6f;PointF q=P(knee.X+(ankle.X-knee.X)*f,knee.Y+(ankle.Y-knee.Y)*f);g.DrawLine(spine,q,P(q.X+side*(2.2f-f),q.Y-1.7f));}
                using(Pen claw=new Pen(Color.FromArgb(220,57,31,18),.55f)){g.DrawLine(claw,toe,P(toe.X+side*1.8f,toe.Y+1.4f));}
            }
            // Long antennae continue probing while the body rests.
            for(int side=-1;side<=1;side+=2)
            {
                float sway=(float)Math.Sin(phase*TAU+side*.8)*9;
                using(GraphicsPath path=new GraphicsPath())
                {
                    path.AddBezier(P(side*7,-54),P(side*(14+sway*.25f),-82),P(side*(29+sway),-120),P(side*(51+sway),-124+(float)Math.Sin(phase*TAU+side)*7));
                    using(Pen pen=new Pen(Color.FromArgb(245,83,45,22),1.0f))g.DrawPath(pen,path);
                    using(Pen pen=new Pen(Color.FromArgb(150,163,112,64),.32f))g.DrawPath(pen,path);
                }
            }
            float swayBody=(float)Math.Sin(phase*TAU*2)*.35f*motion;
            float bodyBob=(float)Math.Abs(Math.Sin(phase*TAU*2))*1.1f*motion;
            g.TranslateTransform(swayBody,bodyBob);
            g.RotateTransform((float)Math.Sin(phase*TAU*2)*.38f*motion);
            g.DrawImage(body,new RectangleF(-23,-61,46,119),crop,GraphicsUnit.Pixel);
        }
        return result;
    }
    Bitmap NymphFrame(float phase,bool walking)
    {
        Bitmap result=new Bitmap(256,256,PixelFormat.Format32bppPArgb);
        using(Graphics g=Graphics.FromImage(result))
        {
            g.Clear(Color.Transparent);g.SmoothingMode=SmoothingMode.AntiAlias;g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.TranslateTransform(128,126);
            for(int side=-1;side<=1;side+=2)for(int leg=0;leg<3;leg++)
            {
                bool firstTripod=(leg%2==0)==(side==1);float cycle=(phase+(firstTripod?0:.5f))%1f;float stride=0,lift=0;
                if(walking){const float stance=.64f;if(cycle<stance)stride=-6+12*cycle/stance;else{float swing=(cycle-stance)/(1-stance);stride=6-12*swing*swing*(3-2*swing);lift=(float)Math.Sin(swing*Math.PI)*3.4f;}}
                float hipY=-17+leg*13,kneeY=hipY+new float[]{-10,1,12}[leg]+stride*.2f-lift*.2f,footY=hipY+new float[]{-18,5,29}[leg]+stride;
                PointF hip=P(side*8,hipY),knee=P(side*(17+lift*.25f),kneeY),ankle=P(side*(28-lift*.4f),footY),toe=P(side*(35-lift*.3f),footY+2+stride*.08f);
                Segment(g,hip,knee,2.0f);Segment(g,knee,ankle,1.05f);Segment(g,ankle,toe,.52f);
                using(Pen spines=new Pen(Color.FromArgb(210,58,31,18),.42f))for(int n=1;n<=4;n++){float f=n/5f;PointF q=P(knee.X+(ankle.X-knee.X)*f,knee.Y+(ankle.Y-knee.Y)*f);g.DrawLine(spines,q,P(q.X+side*1.4f,q.Y-1.2f));}
            }
            for(int side=-1;side<=1;side+=2)
            {
                float sway=(float)Math.Sin(phase*TAU+side*.7f)*5;
                using(GraphicsPath path=new GraphicsPath()){path.AddBezier(P(side*4,-28),P(side*(10+sway*.2f),-44),P(side*(22+sway),-67),P(side*(31+sway),-71+(float)Math.Sin(phase*TAU+side)*4));using(Pen antenna=new Pen(Color.FromArgb(240,74,39,20),.8f))g.DrawPath(antenna,path);using(Pen glint=new Pen(Color.FromArgb(135,177,118,66),.25f))g.DrawPath(glint,path);}
            }
            float bob=walking?(float)Math.Abs(Math.Sin(phase*TAU*2))*.55f:0;g.TranslateTransform(0,bob);g.DrawImage(nymphBody,new RectangleF(-17,-33,34,72),nymphCrop,GraphicsUnit.Pixel);
        }
        return result;
    }
    public void Export(string dir)
    {
        Directory.CreateDirectory(dir);
        using(Bitmap sheet=new Bitmap(256*8,256*((Walk.Length+7)/8),PixelFormat.Format32bppPArgb))
        using(Graphics g=Graphics.FromImage(sheet))
        {for(int i=0;i<Walk.Length;i++){Walk[i].Save(Path.Combine(dir,"walk-"+i.ToString("D2")+".png"));g.DrawImage(Walk[i],new Rectangle((i%8)*256,(i/8)*256,256,256));}sheet.Save(Path.Combine(dir,"walk-sheet.png"));}
        using(Bitmap sample=new Bitmap(1000,380))
        using(Graphics g=Graphics.FromImage(sample))
        {Color[] colors={Color.White,Color.FromArgb(22,27,32),Color.FromArgb(136,156,119),Color.FromArgb(230,213,182)};for(int i=0;i<4;i++){using(Brush b=new SolidBrush(colors[i]))g.FillRectangle(b,i*250,0,250,380);g.DrawImage(Walk[i*11],new Rectangle(i*250,55,250,250));}sample.Save(Path.Combine(dir,"background-check.png"));}
        for(int i=0;i<Pounce.Length;i++)Pounce[i].Save(Path.Combine(dir,"pounce-"+i.ToString("D2")+".png"));
        using(Bitmap sheet=new Bitmap(6*256,4*256,PixelFormat.Format32bppPArgb))using(Graphics g=Graphics.FromImage(sheet)){for(int i=0;i<Pounce.Length;i++)g.DrawImage(Pounce[i],new Rectangle((i%6)*256,(i/6)*256,256,256));sheet.Save(Path.Combine(dir,"pounce-sheet.png"));}
        using(Bitmap sheet=new Bitmap(9*256,8*256,PixelFormat.Format32bppPArgb))using(Graphics g=Graphics.FromImage(sheet)){for(int i=0;i<FrontPounce.Length;i++)g.DrawImage(FrontPounce[i],new Rectangle((i%9)*256,(i/9)*256,256,256));sheet.Save(Path.Combine(dir,"front-pounce-sheet.png"));}
        using(Bitmap sheet=new Bitmap(6*256,4*256,PixelFormat.Format32bppPArgb))using(Graphics g=Graphics.FromImage(sheet)){for(int i=0;i<NymphPounce.Length;i++)g.DrawImage(NymphPounce[i],new Rectangle((i%6)*256,(i/6)*256,256,256));sheet.Save(Path.Combine(dir,"nymph-flight-sheet.png"));}
        for(int i=0;i<Nymph.Length;i++)Nymph[i].Save(Path.Combine(dir,"nymph-"+i.ToString("D2")+".png"));
        for(int i=0;i<Crush.Length;i++)Crush[i].Save(Path.Combine(dir,"crush-"+i.ToString("D2")+".png"));
        using(Bitmap sheet=new Bitmap(4*256,4*256,PixelFormat.Format32bppPArgb))using(Graphics g=Graphics.FromImage(sheet)){for(int i=0;i<NymphCrush.Length;i++)g.DrawImage(NymphCrush[i],new Rectangle((i%4)*256,(i/4)*256,256,256));sheet.Save(Path.Combine(dir,"nymph-crush-sheet.png"));}
        using(Bitmap sheet=new Bitmap(4*256,12*256,PixelFormat.Format32bppPArgb))using(Graphics g=Graphics.FromImage(sheet)){for(int i=0;i<Nymph.Length;i++)g.DrawImage(Nymph[i],new Rectangle((i%4)*256,(i/4)*256,256,256));sheet.Save(Path.Combine(dir,"nymph-sheet.png"));}
        using(Bitmap sheet=new Bitmap(4*256,4*256,PixelFormat.Format32bppPArgb))using(Graphics g=Graphics.FromImage(sheet)){for(int i=0;i<Crush.Length;i++)g.DrawImage(Crush[i],new Rectangle((i%4)*256,(i/4)*256,256,256));sheet.Save(Path.Combine(dir,"crush-sheet.png"));}
        File.WriteAllText(Path.Combine(dir,"info.txt"),"72 adult walking poses + 48 juvenile nymph poses + 24 side-flight frames + 72 frontal-pounce frames at about 24-30 fps with ground crouch, push-off transition, upstroke, downstroke, and articulated legs + 16 adult-crush + 16 nymph-crush frames with full-silhouette atlas bleed and safe fit; nymphs start at minimum scale and grow one stage per minute to default adult scale after six minutes; premultiplied alpha; no color key.");
    }
    static Bitmap NormalizeFrontPose(Bitmap raw,Rectangle referenceBounds)
    {
        const int canvas=384;float scale=Math.Min(270f/referenceBounds.Width,270f/referenceBounds.Height),w=raw.Width*scale,h=raw.Height*scale;
        float anchorX=(referenceBounds.Left+referenceBounds.Right)*.5f,anchorY=(referenceBounds.Top+referenceBounds.Bottom)*.5f;
        ColorMatrix tone=new ColorMatrix(new float[][]{
            new float[]{.84f,.03f,0,0,0},new float[]{.02f,.80f,.02f,0,0},new float[]{0,.04f,.76f,0,0},
            new float[]{0,0,0,1,0},new float[]{0,0,0,0,1}});
        Bitmap pose=new Bitmap(canvas,canvas,PixelFormat.Format32bppPArgb);
        using(Graphics g=Graphics.FromImage(pose))using(ImageAttributes attributes=new ImageAttributes())
        {
            g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;
            attributes.SetColorMatrix(tone,ColorMatrixFlag.Default,ColorAdjustType.Bitmap);
            RectangleF target=new RectangleF(canvas*.5f-anchorX*scale,canvas*.5f-anchorY*scale,w,h);
            g.DrawImage(raw,Rectangle.Round(target),0,0,raw.Width,raw.Height,GraphicsUnit.Pixel,attributes);
        }
        return pose;
    }
    static float FrontScale(int index,bool ground)
    {
        if(ground)return 1f;
        if(index<8)return .78f+.08f*(index-4)/3f;
        if(index<12)return .86f+.08f*(index-8)/3f;
        if(index<FrontLaunchFrames)return .94f+.04f*(index-12)/(FrontLaunchFrames-13f);
        if(index<FrontDiveStart)return .98f+.10f*(index-FrontFlightStart)/(FrontFlightFrames-1f);
        if(index<FrontRecoilStart)return 1.08f+.02f*(float)Math.Sin(Math.PI*(index-FrontDiveStart)/(FrontDiveFrames-1f));
        float recoil=(index-FrontRecoilStart)/(FrontRecoilFrames-1f);
        recoil=recoil*recoil*(3-2*recoil);
        return 1.08f-.30f*recoil;
    }
    static Bitmap CreateFrontPounceFrame(Bitmap basePose,Bitmap upPose,Bitmap downPose,Bitmap crouchPose,Bitmap transitionPose,Bitmap[] groundPoses,int index)
    {
        const int canvas=384;Bitmap pose=basePose;bool ground=false;
        if(index<FrontLaunchFrames)
        {
            if(index<4){pose=groundPoses[(index*11)%groundPoses.Length];ground=true;}
            else if(index<8)pose=crouchPose;
            else if(index<12)pose=transitionPose;
            else pose=upPose;
        }
        else if(index<FrontDiveStart)
        {
            int flapPhase=(index-FrontFlightStart)%6;
            if(flapPhase<2)pose=upPose;
            else if(flapPhase==2)pose=transitionPose;
            else if(flapPhase<5)pose=downPose;
            else pose=basePose;
        }
        else if(index<FrontRecoilStart)
        {
            int divePhase=(index-FrontDiveStart)%4;
            pose=divePhase<2?downPose:divePhase==2?transitionPose:upPose;
        }
        else
        {
            int recoilPhase=index-FrontRecoilStart;
            if(recoilPhase<4)pose=upPose;
            else if(recoilPhase<7)pose=transitionPose;
            else if(recoilPhase==7)pose=crouchPose;
            else{pose=groundPoses[(recoilPhase*13)%groundPoses.Length];ground=true;}
        }
        float size=canvas*FrontScale(index,ground);
        float phase=index<FrontLaunchFrames?index*.36f:index<FrontDiveStart?(index-FrontFlightStart)*(float)(Math.PI*2/6):index<FrontRecoilStart?index*.61f:index*.42f;
        float offsetX=(float)Math.Sin(phase*.7f)*.85f,offsetY=(float)Math.Sin(phase)*1.3f;
        float roll=(float)Math.Sin(phase+.45f)*.4f;
        float stretchX=pose==downPose?1.008f:1.0f,stretchY=pose==upPose?1.008f:1.0f;
        Bitmap frame=new Bitmap(canvas,canvas,PixelFormat.Format32bppPArgb);
        using(Graphics g=Graphics.FromImage(frame))
        {
            g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;
            g.TranslateTransform(canvas*.5f+offsetX,canvas*.5f+offsetY);g.RotateTransform(roll);
            g.DrawImage(pose,new RectangleF(-size*stretchX*.5f,-size*stretchY*.5f,size*stretchX,size*stretchY),new Rectangle(0,0,pose.Width,pose.Height),GraphicsUnit.Pixel);
        }
        return frame;
    }
    public void Dispose(){foreach(Bitmap b in Walk)if(b!=null)b.Dispose();foreach(Bitmap b in Idle)if(b!=null)b.Dispose();foreach(Bitmap b in Pounce)if(b!=null)b.Dispose();foreach(Bitmap b in FrontPounce)if(b!=null)b.Dispose();foreach(Bitmap b in NymphPounce)if(b!=null)b.Dispose();foreach(Bitmap b in Crush)if(b!=null)b.Dispose();foreach(Bitmap b in NymphCrush)if(b!=null)b.Dispose();foreach(Bitmap b in Nymph)if(b!=null)b.Dispose();foreach(Bitmap b in NymphIdle)if(b!=null)b.Dispose();body.Dispose();nymphBody.Dispose();}
}

sealed class Pet : Form
{
    public const float DefaultAdultScale=.75f;
    public const float MinimumNymphScale=.24f;
    readonly Rig rig;
    readonly SwarmController swarm;
    readonly System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer();
    readonly Stopwatch clock=Stopwatch.StartNew();
    readonly Random random=new Random();
    readonly Bitmap buffer=new Bitmap(320,320,PixelFormat.Format32bppPArgb);
    readonly NotifyIcon tray=new NotifyIcon();
    readonly ContextMenuStrip menu=new ContextMenuStrip();
    readonly ToolStripMenuItem pause=new ToolStripMenuItem("暂停爬行");
    readonly ToolStripMenuItem affectionLabel=new ToolStripMenuItem();
    readonly ToolStripMenuItem feedItem=new ToolStripMenuItem();
    readonly ToolStripMenuItem demoPounceItem=new ToolStripMenuItem("立即演示飞扑");
    readonly ToolStripMenuItem quietActivityItem=new ToolStripMenuItem("安静"),naturalActivityItem=new ToolStripMenuItem("自然"),activeActivityItem=new ToolStripMenuItem("活跃");
    readonly CrackEffect crack=new CrackEffect();
    int affection;
    double x,y,heading=-Math.PI/2,desired=-Math.PI/2,speed,targetSpeed,phase,idlePhase,timeLeft=1,last,turnNoise,scareCooldown,nextSpecial,mealX,mealY,mealUntil,mealDeadline,flightStart,flightDuration,frontalFlightDuration,happyStart,crushStart,impactUntil,lastEvadeAt;
    float petScale=DefaultAdultScale,nymphScale=MinimumNymphScale,nymphBirthScale=MinimumNymphScale,pace=1.5f,pounceSpeed=1f;
    bool paused,dragging,menuOpen,mealActive,eating,crashQueued,suppressMouseUp,manualPounce,isNymph,crushTransferred;
    int nymphGeneration,evadeCount;
    double nymphBirthAt;
    int mealGain;
    Point dragOffset,previousCursor;
    string mode="probe";
    Rectangle area;
    readonly EventWaitHandle recall;
    double revealUntil;
    readonly bool smoke;
    DateTime lastClickAt=DateTime.MinValue;
    Point lastClickPosition;
    int presented;
    double startX,startY,traveled;
    protected override bool ShowWithoutActivation {get{return true;}}
    protected override CreateParams CreateParams {get{CreateParams p=base.CreateParams;p.ExStyle|=0x80000|0x80|0x08000000;return p;}}
    public Pet(Rig rig,bool smoke,EventWaitHandle recall)
    {
        this.rig=rig;this.swarm=new SwarmController(rig);this.smoke=smoke;this.recall=recall;Text="小强桌宠 · 动态版";FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;TopMost=true;Size=new Size(320,320);StartPosition=FormStartPosition.Manual;
        area=Screen.PrimaryScreen.WorkingArea;x=area.Left+area.Width*.65;y=area.Top+area.Height*.65;
        startX=x;startY=y;targetSpeed=32;Location=new Point((int)x-160,(int)y-160);previousCursor=Cursor.Position;
        affection=Program.ReadAffinity();pause.Click+=delegate {paused=!paused;pause.Text=paused?"继续爬行":"暂停爬行";};menu.Items.Add(pause);
        affectionLabel.Enabled=false;UpdateAffectionLabel();menu.Items.Add(affectionLabel);
        feedItem.Text="投喂一粒面包屑";feedItem.Click+=delegate{Feed();};menu.Items.Add(feedItem);
        demoPounceItem.Click+=delegate{StartDemoPounce();};menu.Items.Add(demoPounceItem);
        ToolStripMenuItem sizes=new ToolStripMenuItem("大小");Add(sizes,"小 · 接近实物",delegate{petScale=.55f;});Add(sizes,"中",delegate{petScale=.75f;});Add(sizes,"大",delegate{petScale=1.0f;});menu.Items.Add(sizes);
        ToolStripMenuItem speeds=new ToolStripMenuItem("活跃程度");
        quietActivityItem.Click+=delegate{SetPace(.65f);};naturalActivityItem.Click+=delegate{SetPace(1f);};activeActivityItem.Click+=delegate{SetPace(1.5f);};
        speeds.DropDownItems.Add(quietActivityItem);speeds.DropDownItems.Add(naturalActivityItem);speeds.DropDownItems.Add(activeActivityItem);SetPace(1.5f);menu.Items.Add(speeds);
        menu.Items.Add("回到屏幕中央",null,delegate {Reveal();});
        menu.Items.Add(new ToolStripSeparator());menu.Items.Add("退出桌宠",null,delegate {Close();});
        menu.Opening+=delegate{menuOpen=true;};menu.Closed+=delegate{menuOpen=false;};
        tray.Icon=SystemIcons.Application;tray.Text="小强桌宠 · 右键控制，左键拖动";tray.ContextMenuStrip=menu;tray.Visible=true;
        tray.DoubleClick+=delegate{Reveal();};
        timer.Interval=16;timer.Tick+=Tick;
        menu.Opening+=delegate{UpdateAffectionLabel();};
        Shown+=delegate{last=clock.Elapsed.TotalSeconds;nextSpecial=last+Rand(20,50);if(!smoke){Show();Present();}else{Present();}timer.Start();};
    }
    void Reveal()
    {
        area=Screen.FromPoint(Cursor.Position).WorkingArea;x=area.Left+area.Width*.5;y=area.Top+area.Height*.5;
        paused=false;pause.Text="暂停爬行";dragging=false;Capture=false;speed=0;targetSpeed=0;timeLeft=3;revealUntil=clock.Elapsed.TotalSeconds+3;scareCooldown=4;previousCursor=Cursor.Position;
        Show();TopMost=true;Present();
        tray.ShowBalloonTip(2500,"小强桌宠已启动","已回到当前屏幕中央。右键控制，再次双击程序可召回。",ToolTipIcon.Info);
        Program.Log("REVEAL visible="+Visible+" center="+x+","+y+" area="+area+" alpha="+buffer.GetPixel(0,0).A);
    }
    static void Add(ToolStripMenuItem parent,string text,Action action){parent.DropDownItems.Add(text,null,delegate{action();});}
    void SetPace(float value){pace=value;quietActivityItem.Checked=value==.65f;naturalActivityItem.Checked=value==1f;activeActivityItem.Checked=value==1.5f;}
    string AffinityMood(){return affection<10?"陌生":affection<30?"认得你":affection<60?"熟悉":affection<85?"喜欢":"很亲近";}
    void UpdateAffectionLabel(){affectionLabel.Text="好感度 "+affection+" / 100 · "+AffinityMood();feedItem.Text="投喂一粒面包屑";feedItem.Enabled=!mealActive;demoPounceItem.Enabled=!isNymph;}
    void Feed()
    {
        if(mealActive)return;
        mealActive=true;eating=false;mealGain=3+random.Next(0,3);
        double margin=75*petScale;mealX=Math.Max(area.Left+margin,Math.Min(area.Right-margin,x+Math.Cos(heading)*83));mealY=Math.Max(area.Top+margin,Math.Min(area.Bottom-margin,y+Math.Sin(heading)*83));mealDeadline=clock.Elapsed.TotalSeconds+25;mealUntil=0;
        mode="food";targetSpeed=Rand(35,55);desired=Math.Atan2(mealY-y,mealX-x);UpdateAffectionLabel();
        Program.Log("Meal offered; planned gain="+mealGain+".");
    }
    void FinishMeal(double now)
    {
        mealActive=false;eating=false;affection=Math.Min(100,affection+mealGain);Program.SaveAffinity(affection);
        mode="happy";happyStart=now;timeLeft=affection<20?1.5:affection<50?2.1:affection<80?2.8:3.5;speed=0;targetSpeed=0;desired=heading;nextSpecial=now+Rand(12,26);UpdateAffectionLabel();
        Program.Log("Meal complete; gain="+mealGain+" affection="+affection+".");
    }
    void StartDemoPounce()
    {
        if(isNymph)return;
        area=Screen.FromPoint(Cursor.Position).WorkingArea;paused=false;pause.Text="暂停爬行";mealActive=false;eating=false;
        double now=clock.Elapsed.TotalSeconds;pounceSpeed=ChoosePounceSpeed();frontalFlightDuration=1.15/pounceSpeed;mode="launch";flightStart=now;timeLeft=.48/pounceSpeed;targetSpeed=0;desired=heading;crashQueued=true;manualPounce=true;scareCooldown=3;revealUntil=now;
        speed=Math.Max(speed,40);menuOpen=false;
        Program.Log("Manual frontal fly-pounce started; speed="+PounceSpeedName()+".");Present();
    }
    float ChoosePounceSpeed(){double roll=random.NextDouble();return roll<.25?1.7f:roll<.6?1.3f:1f;}
    string PounceSpeedName(){return pounceSpeed>=1.6f?"very fast":pounceSpeed>=1.2f?"fast":"normal";}
    void StartCrush()
    {
        if(mode=="crush")return;
        area=Screen.FromPoint(Cursor.Position).WorkingArea;paused=false;pause.Text="暂停爬行";mealActive=false;eating=false;
        double now=clock.Elapsed.TotalSeconds;mode="crush";crushStart=now;crushTransferred=false;timeLeft=1.05;speed=0;targetSpeed=0;manualPounce=false;crashQueued=false;revealUntil=now;menuOpen=false;UpdateAffectionLabel();
        Program.Log("Double-click crush animation started; nymph="+isNymph+".");Present();
    }
    double Rand(double a,double b){return a+random.NextDouble()*(b-a);}
    static double Wrap(double a){while(a>Math.PI)a-=Math.PI*2;while(a< -Math.PI)a+=Math.PI*2;return a;}
    void ChooseBehavior()
    {
        double chance=random.NextDouble();
        if(chance<.32){mode="rest";targetSpeed=0;timeLeft=Rand(.6,2.6);}
        else if(chance<.75){mode="probe";targetSpeed=Rand(18,48)*pace;timeLeft=Rand(.6,1.8);desired=heading+Rand(-1.0,1.0);}
        else{mode="dash";targetSpeed=Rand(115,205)*pace;timeLeft=Rand(.35,.9);desired=heading+Rand(-.5,.5);}
    }
    void Tick(object sender,EventArgs args)
    {
        double now=clock.Elapsed.TotalSeconds,dt=Math.Min(.05,now-last);last=now;idlePhase=(idlePhase+dt*.38)%1;
        if(evadeCount>0&&now-lastEvadeAt>=5)evadeCount=0;
        if(isNymph){int stage=Math.Min(6,(int)(Math.Max(0,now-nymphBirthAt)/60));nymphScale=nymphBirthScale+(DefaultAdultScale-nymphBirthScale)*stage/6f;}
        if(recall.WaitOne(0))Reveal();
        if(mode=="crush"){speed=0;targetSpeed=0;}
        else if(!paused&&!dragging&&!menuOpen && now>=revealUntil)
        {
            timeLeft-=dt;scareCooldown-=dt;
            if(mealActive)
            {
                if(!eating && now>mealDeadline){mealActive=false;targetSpeed=0;UpdateAffectionLabel();Program.Log("Meal expired before pickup.");}
                else if(!eating){desired=Math.Atan2(mealY-y,mealX-x);double fd=Math.Sqrt((mealX-x)*(mealX-x)+(mealY-y)*(mealY-y));if(fd<31){eating=true;speed=0;targetSpeed=0;mealUntil=now+1.15;}else targetSpeed=Math.Min(78,fd*1.25);}
                else {targetSpeed=0;if(now>=mealUntil)FinishMeal(now);}
            }
            else if(timeLeft<=0)
            {
                if(mode=="launch"){mode="flight";flightStart=now;timeLeft=crashQueued?frontalFlightDuration:Rand(1.7,2.5);targetSpeed=manualPounce?70*pounceSpeed:Rand(180,250);desired=heading+(manualPounce?0:Rand(-.85,.85));}
                else if(mode=="flight" && crashQueued){mode="dive";flightStart=now;timeLeft=.55/pounceSpeed;targetSpeed=(manualPounce?120:290)*pounceSpeed;desired=heading;}
                else if(mode=="dive"){mode="recoil";flightStart=now;timeLeft=.42/pounceSpeed;speed=0;targetSpeed=0;impactUntil=now+1.35;crack.Show((int)x,(int)y,now);}
                else if(mode=="recoil" || mode=="flight"){mode="probe";targetSpeed=Rand(25,55);timeLeft=Rand(.8,1.5);crashQueued=false;manualPounce=false;nextSpecial=now+(isNymph?Rand(2.5,5.5):Rand(15,35));}
                else if(mode=="happy"){ChooseBehavior();}
                else if(isNymph && now>=nextSpecial)
                {
                    nextSpecial=now+Rand(2.5,5.5);
                    if(random.NextDouble()<.34){mode="flight";flightStart=now;flightDuration=Rand(.85,1.35);timeLeft=flightDuration;targetSpeed=Rand(95,155);desired=heading+Rand(-1.3,1.3);crashQueued=false;manualPounce=false;}
                    else ChooseBehavior();
                }
                else if(!isNymph && affection>=10 && now>=nextSpecial){double odds=.0015+affection*.00012;nextSpecial=now+Rand(.75,1.25);if(random.NextDouble()<odds){crashQueued=affection>=20 && random.NextDouble()<(.04+affection*.003);pounceSpeed=crashQueued?ChoosePounceSpeed():1f;mode=crashQueued?"launch":"flight";flightStart=now;frontalFlightDuration=crashQueued?Rand(1.0,1.4)/pounceSpeed:0;timeLeft=crashQueued ? .48/pounceSpeed : Rand(1.7,2.5);targetSpeed=crashQueued?0:Rand(180,250);desired=heading+Rand(-.85,.85);}else ChooseBehavior();}
                else ChooseBehavior();
            }
            Point cursor=Cursor.Position;double cx=x-cursor.X,cy=y-cursor.Y;
            double cursorMovement=Math.Abs(cursor.X-previousCursor.X)+Math.Abs(cursor.Y-previousCursor.Y);
            if(!mealActive && cx*cx+cy*cy<10000 && cursorMovement>3 && scareCooldown<=0)
            {
                if(now-lastEvadeAt>5)evadeCount=0;evadeCount++;lastEvadeAt=now;
                double crashOdds=!isNymph&&evadeCount>=3?Math.Min(.7,.12+(evadeCount-3)*.1):0;
                if(crashOdds>0&&random.NextDouble()<crashOdds){pounceSpeed=ChoosePounceSpeed();mode="launch";flightStart=now;frontalFlightDuration=Rand(.9,1.3)/pounceSpeed;timeLeft=.48/pounceSpeed;targetSpeed=0;desired=heading+Rand(-.35,.35);crashQueued=true;manualPounce=false;scareCooldown=3;Program.Log("Repeated mouse avoidance triggered a "+PounceSpeedName()+" frontal flight-crash; escapes="+evadeCount+".");}
                else
                {
                    double response=random.NextDouble();double away=Math.Atan2(cy,cx)+Rand(-.5,.5);
                    if(response<.18){mode="rest";desired=heading+Rand(-1.2,1.2);targetSpeed=0;timeLeft=Rand(.35,.8);scareCooldown=Rand(1.6,2.4);}
                    else if(response<.68){mode="escape";desired=away;targetSpeed=Rand(145,215)*pace;timeLeft=Rand(.35,.7);scareCooldown=Rand(1.8,2.7);}
                    else{mode="probe";desired=away+Rand(-1.0,1.0);targetSpeed=Rand(35,78)*pace;timeLeft=Rand(.4,.9);scareCooldown=Rand(1.8,2.7);}
                }
            }
            previousCursor=cursor;
            // Steer before hitting the edge instead of snapping or bouncing at a corner.
            bool frontalCrash=!isNymph&&crashQueued&&(mode=="launch"||mode=="flight"||mode=="dive"||mode=="recoil");
            double margin=frontalCrash?Math.Min(168*petScale,Math.Min(area.Width,area.Height)*.45):80*petScale,look=margin+speed*.35;
            double nx=Math.Cos(heading),ny=Math.Sin(heading);
            if(x+nx*look<area.Left+margin || x+nx*look>area.Right-margin || y+ny*look<area.Top+margin || y+ny*look>area.Bottom-margin)
            {desired=Math.Atan2((area.Top+area.Height*.5)-y,(area.Left+area.Width*.5)-x)+Math.Sin(now*.9)*.3;timeLeft=Math.Max(timeLeft,.6);if(targetSpeed==0)targetSpeed=25;}
            turnNoise+=(Rand(-1,1)-turnNoise)*dt*3;
            double error=Wrap(desired+turnNoise*.18-heading);
            double maxTurn=(mode=="escape"?7:mode=="dive"?5:mode=="dash"||mode=="flight"?3.5:2.7)*dt;
            if(targetSpeed>0)heading+=Math.Max(-maxTurn,Math.Min(maxTurn,error));
            double wanted=targetSpeed*(1-Math.Min(.65,Math.Abs(error)*.22));
            speed+=(wanted-speed)*Math.Min(1,dt*(wanted>speed?5:9));
            double distance=speed*dt;
            double oldX=x,oldY=y;x=Math.Max(area.Left+margin,Math.Min(area.Right-margin,x+Math.Cos(heading)*distance));y=Math.Max(area.Top+margin,Math.Min(area.Bottom-margin,y+Math.Sin(heading)*distance));
            double actual=Math.Sqrt((x-oldX)*(x-oldX)+(y-oldY)*(y-oldY));traveled+=actual;phase=(phase+actual/(27*petScale))%1;
        }
        if(mode=="crush"&&!crushTransferred&&now-crushStart>=1.05)
        {
            crushTransferred=true;
            if(!isNymph){isNymph=true;nymphGeneration=1;}else{nymphGeneration++;}
            nymphScale=MinimumNymphScale;
            nymphBirthScale=nymphScale;nymphBirthAt=now;
            mode="nymph";speed=0;targetSpeed=Rand(28,58);desired=heading+Rand(-1.4,1.4);timeLeft=Rand(.55,1.4);
            nextSpecial=now+Rand(2.5,5.5);
            swarm.SpawnBurst((int)x,(int)y,nymphGeneration,MinimumNymphScale,area,10);
            UpdateAffectionLabel();Program.Log("Crush split into a roaming nymph and a small swarm.");
        }
        Present();
        if(smoke && now>6)
        {
            string dir=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"preview");Directory.CreateDirectory(dir);
            buffer.Save(Path.Combine(dir,"live-buffer.png"));
            File.WriteAllText(Path.Combine(dir,"smoke.txt"),String.Format("UpdateLayeredWindow successful: {0} frames\r\nTravel: {1:F1} px\r\nStart: {2:F0},{3:F0}\r\nEnd: {4:F0},{5:F0}\r\nCorner alpha: {6}\r\nWorking area: {7}\r\n",presented,traveled,startX,startY,x,y,buffer.GetPixel(0,0).A,area));
            Close();
        }
    }
    void Present()
    {
        bool moving=!paused&&!dragging&&!menuOpen&&speed>3;
        double now=clock.Elapsed.TotalSeconds,happyAge=Math.Max(0,now-happyStart);
        bool frontAttack=!isNymph&&crashQueued&&(mode=="launch"||mode=="flight"||mode=="dive"||mode=="recoil");
        Bitmap frame;
        if(mode=="crush")frame=isNymph?rig.NymphCrush[Math.Min(rig.NymphCrush.Length-1,(int)(Math.Max(0,now-crushStart)/1.05*rig.NymphCrush.Length))]:rig.Crush[Math.Min(rig.Crush.Length-1,(int)(Math.Max(0,now-crushStart)/1.05*rig.Crush.Length))];
        else if(isNymph && mode=="flight")frame=rig.NymphPounce[SmallFlightFrame(now-flightStart,flightDuration)];
        else if(isNymph)frame=mode=="happy"?rig.Nymph[(int)(happyAge*14)%rig.Nymph.Length]:moving?rig.Nymph[(int)(phase*rig.Nymph.Length)%rig.Nymph.Length]:rig.NymphIdle[(int)(idlePhase*rig.NymphIdle.Length)%rig.NymphIdle.Length];
        else if(mode=="launch")frame=frontAttack?rig.FrontPounce[Math.Min(Rig.FrontLaunchFrames-1,(int)(Math.Max(0,now-flightStart)/Math.Max(.01,.48/pounceSpeed)*Rig.FrontLaunchFrames))]:rig.Pounce[Math.Min(2,(int)(Math.Max(0,now-flightStart)*6.25))];
        else if(mode=="flight")frame=frontAttack?rig.FrontPounce[Rig.FrontFlightStart+Math.Min(Rig.FrontFlightFrames-1,(int)(Math.Max(0,now-flightStart)/Math.Max(.01,frontalFlightDuration)*Rig.FrontFlightFrames))]:rig.Pounce[3+((int)(Math.Max(0,now-flightStart)*14)%15)];
        else if(mode=="dive")frame=frontAttack?rig.FrontPounce[Rig.FrontDiveStart+Math.Min(Rig.FrontDiveFrames-1,(int)(Math.Max(0,now-flightStart)/Math.Max(.01,.55/pounceSpeed)*Rig.FrontDiveFrames))]:rig.Pounce[Math.Min(21,18+(int)(Math.Max(0,now-flightStart)/.55*4))];
        else if(mode=="recoil")frame=frontAttack?rig.FrontPounce[Rig.FrontRecoilStart+Math.Min(Rig.FrontRecoilFrames-1,(int)(Math.Max(0,now-flightStart)/Math.Max(.01,.42/pounceSpeed)*Rig.FrontRecoilFrames))]:rig.Pounce[Math.Min(23,22+(int)(Math.Max(0,now-flightStart)*5))];
        else if(mode=="happy" && affection<20)frame=rig.Walk[(int)(happyAge*16)%rig.Walk.Length];
        else if(mode=="happy")
        {
            int first=affection<50?3:8,count=affection<50?5:affection<80?10:10,rate=affection<50?8:affection<80?10:14;
            frame=rig.Pounce[first+((int)(happyAge*rate)%count)];
        }
        else frame=moving?rig.Walk[(int)(phase*rig.Walk.Length)%rig.Walk.Length]:rig.Idle[(int)(idlePhase*rig.Idle.Length)%rig.Idle.Length];
        using(Graphics g=Graphics.FromImage(buffer))
        {
            g.Clear(Color.Transparent);g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.PixelOffsetMode=PixelOffsetMode.HighQuality;
            g.TranslateTransform(160,160);
            if(!frontAttack)g.RotateTransform((float)(heading*180/Math.PI+90));
            else if(mode=="launch")
            {
                float t=(float)Math.Max(0,Math.Min(1,(now-flightStart)/Math.Max(.01,.48/pounceSpeed))),turn=1-t*t*(3-2*t);
                g.RotateTransform((float)(heading*180/Math.PI+90)*turn);
            }
            else if(mode=="recoil")
            {
                float t=(float)Math.Max(0,Math.Min(1,(now-flightStart-.25/pounceSpeed)/Math.Max(.01,.17/pounceSpeed))),turn=t*t*(3-2*t);
                g.RotateTransform((float)(heading*180/Math.PI+90)*turn);
            }
            float happyBob=0,happySway=0;
            if(mode=="happy")
            {
                float rate=affection<20?8:affection<50?10:affection<80?12:16;
                float wave=(float)Math.Abs(Math.Sin(happyAge*rate));happyBob=wave*(affection<20?2:affection<50?4:affection<80?7:10);
                happySway=(float)Math.Sin(happyAge*rate*.5)*(affection<20?1.0f:affection<50?1.7f:2.6f);
                g.RotateTransform(happySway);
            }
            float zoom=mode=="launch"?.98f:mode=="flight"?1:mode=="dive"?1.05f:mode=="recoil"?1:1;
            float shake=mode=="recoil"?(float)(Math.Max(0,impactUntil-now)*2.2):0;
            g.TranslateTransform((mode=="dive"||mode=="recoil")?(float)Math.Sin(now*43)*shake:0,-happyBob+((mode=="dive"||mode=="recoil")?(float)Math.Cos(now*51)*shake:0));
            float frontalZoom=1f;
            if(frontAttack)
            {
                if(mode=="launch"){float t=(float)Math.Max(0,Math.Min(1,(now-flightStart)/Math.Max(.01,.48/pounceSpeed)));frontalZoom=1f+.45f*t*t*(3-2*t);}
                else if(mode=="recoil"){float t=(float)Math.Max(0,Math.Min(1,(now-flightStart)/Math.Max(.01,.42/pounceSpeed)));frontalZoom=1.45f-.45f*t*t*(3-2*t);}
                else frontalZoom=1.45f;
            }
            float size=256*(isNymph?nymphScale:petScale)*zoom*frontalZoom;if(frontAttack)size=Math.Min(320,size);float flightLift=isNymph&&mode=="flight"?(float)Math.Sin(Math.PI*Math.Max(0,Math.Min(1,(now-flightStart)/Math.Max(.01,flightDuration))))*9:0;
            g.TranslateTransform(0,-flightLift);g.DrawImage(frame,new RectangleF(-size/2,-size/2,size,size));
            g.ResetTransform();
            if(mealActive)DrawCrumb(g,(float)(160+mealX-x),(float)(160+mealY-y));
        }
        Native.Show(Handle,buffer,(int)Math.Round(x)-160,(int)Math.Round(y)-160);
        crack.Update((float)Math.Max(0,(impactUntil-now)/1.35),Math.Max(0,now-crack.StartedAt));
        presented++;
        if(presented==120)Program.Log("Rendering confirmed: 120 successful layered-window frames.");
    }
    internal static int SmallFlightFrame(double age,double duration)
    {
        if(age<.2)return Math.Min(3,(int)(Math.Max(0,age)/.2*4));
        if(age>=duration-.22)return 18+Math.Min(5,(int)(Math.Max(0,age-(duration-.22))/.22*6));
        return 3+((int)((age-.2)*20)%15);
    }
    static void DrawCrumb(Graphics g,float cx,float cy)
    {
        PointF[] points={new PointF(cx-9,cy-1),new PointF(cx-7,cy-4),new PointF(cx-4,cy-4.5f),new PointF(cx-2,cy-7),new PointF(cx+1,cy-5),new PointF(cx+5,cy-5.5f),new PointF(cx+7,cy-2),new PointF(cx+9,cy),new PointF(cx+6,cy+2),new PointF(cx+5,cy+5),new PointF(cx+1,cy+4),new PointF(cx-2,cy+6),new PointF(cx-4,cy+3),new PointF(cx-8,cy+3)};
        using(SolidBrush shadow=new SolidBrush(Color.FromArgb(72,25,17,10)))g.FillEllipse(shadow,cx-8,cy+2,17,7);
        using(GraphicsPath path=new GraphicsPath())
        {
            path.AddClosedCurve(points,.48f);
            using(PathGradientBrush crust=new PathGradientBrush(path)){crust.CenterColor=Color.FromArgb(255,209,157,91);crust.SurroundColors=new Color[]{Color.FromArgb(255,132,81,42)};crust.CenterPoint=new PointF(cx-1,cy-1);g.FillPath(crust,path);}
            GraphicsState saved=g.Save();g.SetClip(path);
            for(int i=0;i<24;i++)
            {
                float px=cx+(float)Math.Sin(i*12.9898)*7.2f,py=cy+(float)Math.Cos(i*7.123)*3.9f,size=.45f+(i%4)*.18f;
                Color color=i%3==0?Color.FromArgb(205,105,62,32):Color.FromArgb(205,233,190,132);
                using(SolidBrush grain=new SolidBrush(color))g.FillEllipse(grain,px,py,size,size*.72f);
            }
            g.Restore(saved);
            using(Pen edge=new Pen(Color.FromArgb(185,91,55,31),.8f))g.DrawPath(edge,path);
            using(Pen highlight=new Pen(Color.FromArgb(125,255,223,169),.7f))g.DrawArc(highlight,cx-5,cy-4,9,5,195,100);
        }
    }
    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if(e.Button==MouseButtons.Right){menu.Show(Cursor.Position);return;}
        if(e.Button==MouseButtons.Left)
        {
            Point position=Cursor.Position;DateTime now=DateTime.UtcNow;Size threshold=SystemInformation.DoubleClickSize;
            bool isDouble=lastClickAt!=DateTime.MinValue && (now-lastClickAt).TotalMilliseconds<=SystemInformation.DoubleClickTime && Math.Abs(position.X-lastClickPosition.X)<=threshold.Width && Math.Abs(position.Y-lastClickPosition.Y)<=threshold.Height;
            if(isDouble){lastClickAt=DateTime.MinValue;dragging=false;Capture=false;suppressMouseUp=true;StartCrush();return;}
            lastClickAt=now;lastClickPosition=position;dragging=true;Capture=true;dragOffset=new Point(position.X-(int)x,position.Y-(int)y);speed=0;
        }
    }
    protected override void OnMouseMove(MouseEventArgs e){base.OnMouseMove(e);if(dragging){x=Cursor.Position.X-dragOffset.X;y=Cursor.Position.Y-dragOffset.Y;area=Screen.FromPoint(Cursor.Position).WorkingArea;}}
    protected override void OnMouseUp(MouseEventArgs e){base.OnMouseUp(e);if(e.Button==MouseButtons.Left){if(suppressMouseUp){suppressMouseUp=false;return;}dragging=false;Capture=false;timeLeft=.4;targetSpeed=0;scareCooldown=1;}}
    protected override void OnMouseCaptureChanged(EventArgs e){base.OnMouseCaptureChanged(e);if(!Capture)dragging=false;}
    protected override void OnFormClosed(FormClosedEventArgs e){timer.Stop();timer.Dispose();swarm.Dispose();Native.Release(Handle);crack.Close();crack.Dispose();tray.Visible=false;tray.Dispose();menu.Dispose();buffer.Dispose();base.OnFormClosed(e);}
}

sealed class SwarmController : IDisposable
{
    const int MAX_NYMPHS=72;
    readonly Rig rig;
    readonly Random random=new Random();
    readonly Stopwatch clock=Stopwatch.StartNew();
    readonly System.Windows.Forms.Timer updateTimer=new System.Windows.Forms.Timer();
    readonly List<RoachCloneWindow> clones=new List<RoachCloneWindow>();
    readonly List<Burst> pending=new List<Burst>();
    double lastUpdate;
    bool disposed;
    sealed class Burst{public int X,Y,Generation,Count;public float Scale;public Rectangle Area;public double Due;}
    public IList<RoachCloneWindow> Roaches{get{return clones;}}
    public double Now{get{return clock.Elapsed.TotalSeconds;}}
    public SwarmController(Rig rig){this.rig=rig;lastUpdate=Now;updateTimer.Interval=33;updateTimer.Tick+=Update;}
    void Update(object sender,EventArgs args)
    {
        if(disposed)return;
        double now=Now,dt=Math.Min(.12,now-lastUpdate);lastUpdate=now;
        RoachCloneWindow[] snapshot=clones.ToArray();
        foreach(RoachCloneWindow clone in snapshot)clone.Advance(now,dt);
        Pump();
        int count=clones.Count;if(count==0&&pending.Count==0)updateTimer.Stop();else updateTimer.Interval=count>=48?66:count>=24?50:33;
    }
    public void SpawnBurst(int x,int y,int generation,float scale,Rectangle area,int count)
    {
        if(disposed)return;
        if(count<=0)count=8+random.Next(5);
        pending.Add(new Burst{X=x,Y=y,Generation=generation,Scale=scale,Area=area,Count=count,Due=Now});Pump();
        if((clones.Count>0||pending.Count>0)&&!updateTimer.Enabled){lastUpdate=Now;updateTimer.Interval=clones.Count>=48?66:clones.Count>=24?50:33;updateTimer.Start();}
    }
    public void SplitFromClone(RoachCloneWindow parent)
    {
        if(disposed)return;
        int x=(int)parent.WorldX,y=(int)parent.WorldY,generation=parent.Generation+1;float scale=Pet.MinimumNymphScale;Rectangle area=parent.WorkingArea;
        clones.Remove(parent);parent.CloseFromController();
        SpawnBurst(x,y,generation,scale,area,0);
        Program.Log("Nymph crushed; generation="+generation+" active="+clones.Count+".");
    }
    public void Retired(RoachCloneWindow clone){clones.Remove(clone);}
    public void Pump()
    {
        if(disposed)return;double now=Now;
        for(int i=pending.Count-1;i>=0;i--)
        {
            Burst burst=pending[i];if(now<burst.Due)continue;
            int excess=clones.Count+burst.Count-MAX_NYMPHS;
            if(excess>0)
            {
                int scheduled=0;
                foreach(RoachCloneWindow old in clones)
                {if(scheduled>=excess)break;if(!old.IsRoaming)continue;old.BeginRetire();scheduled++;}
                burst.Due=now+(scheduled>0?.42:.2);continue;
            }
            pending.RemoveAt(i);CreateChildren(burst);
        }
    }
    void CreateChildren(Burst b)
    {
        for(int i=0;i<b.Count;i++)
        {
            double angle=(Math.PI*2*i/b.Count)+(random.NextDouble()-.5)*.5,distance=22+random.NextDouble()*52;
            int x=(int)Math.Max(b.Area.Left+42,Math.Min(b.Area.Right-42,b.X+Math.Cos(angle)*distance));
            int y=(int)Math.Max(b.Area.Top+42,Math.Min(b.Area.Bottom-42,b.Y+Math.Sin(angle)*distance));
            RoachCloneWindow clone=new RoachCloneWindow(rig,this,x,y,b.Area,b.Generation,b.Scale);clones.Add(clone);clone.Show();clone.TopMost=true;
        }
        Program.Log("Spawned "+b.Count+" nymphs; active="+clones.Count+".");
    }
    public void Dispose()
    {
        if(disposed)return;disposed=true;updateTimer.Stop();updateTimer.Dispose();pending.Clear();RoachCloneWindow[] all=clones.ToArray();clones.Clear();foreach(RoachCloneWindow c in all)c.CloseFromController();
    }
}

sealed class RoachCloneWindow : Form
{
    readonly Rig rig;
    readonly SwarmController swarm;
    readonly Random random=new Random(Guid.NewGuid().GetHashCode());
    readonly Bitmap buffer=new Bitmap(112,112,PixelFormat.Format32bppPArgb);
    readonly Graphics graphics;
    readonly float birthScale;
    readonly double birthTime;
    readonly Rectangle area;
    double x,y,heading,desired,speed,targetSpeed,phase,nextTurn,actionStart,goalX,goalY,flightStart,flightDuration,nextFlightAt;
    DateTime lastClick=DateTime.MinValue;
    int state; // 0 roaming, 1 crushed, 2 fading out
    bool closed,hasRoamGoal,airborne;
    public int Generation{get;private set;}
    public float PetScale{get;private set;}
    public double WorldX{get{return x;}}
    public double WorldY{get{return y;}}
    public Rectangle WorkingArea{get{return area;}}
    public bool IsRoaming{get{return state==0;}}
    protected override bool ShowWithoutActivation{get{return true;}}
    protected override CreateParams CreateParams{get{CreateParams p=base.CreateParams;p.ExStyle|=0x80000|0x80|0x08000000;return p;}}
    public RoachCloneWindow(Rig rig,SwarmController swarm,int x,int y,Rectangle area,int generation,float scale)
    {
        this.rig=rig;this.swarm=swarm;this.x=x;this.y=y;this.area=area;Generation=generation;birthScale=scale;PetScale=scale;birthTime=swarm.Now;
        Size=new Size(112,112);FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;TopMost=true;StartPosition=FormStartPosition.Manual;Location=new Point(x-56,y-56);heading=random.NextDouble()*Math.PI*2;desired=heading;targetSpeed=Rand(75,170);nextTurn=Rand(.25,.8);nextFlightAt=Rand(1.8,4.2);PickRoamGoal();
        graphics=Graphics.FromImage(buffer);graphics.SmoothingMode=SmoothingMode.AntiAlias;graphics.InterpolationMode=InterpolationMode.Bilinear;graphics.PixelOffsetMode=PixelOffsetMode.Half;
        Shown+=delegate{Draw(swarm.Now);};
    }
    double Rand(double a,double b){return a+random.NextDouble()*(b-a);}
    void PickRoamGoal()
    {
        double left=area.Left+48,right=area.Right-48,top=area.Top+48,bottom=area.Bottom-48;
        if(right<left){left=area.Left;right=area.Right;}if(bottom<top){top=area.Top;bottom=area.Bottom;}
        goalX=Rand(left,right);goalY=Rand(top,bottom);hasRoamGoal=true;
    }
    static double Wrap(double angle){while(angle>Math.PI)angle-=Math.PI*2;while(angle< -Math.PI)angle+=Math.PI*2;return angle;}
    void BeginCrush(){if(state!=0)return;state=1;airborne=false;actionStart=swarm.Now;speed=targetSpeed=0;}
    public void BeginRetire(){if(state!=0)return;state=2;airborne=false;actionStart=swarm.Now;speed=targetSpeed=0;}
    public void CloseFromController(){if(closed)return;closed=true;Close();}
    public void Advance(double now,double dt)
    {
        if(closed)return;
        if(state==0){int growthStage=Math.Min(6,(int)(Math.Max(0,now-birthTime)/60));PetScale=birthScale+(Pet.DefaultAdultScale-birthScale)*growthStage/6f;}
        if(state==1&&now-actionStart>=1.0){swarm.SplitFromClone(this);return;}
        if(state==2&&now-actionStart>=.38){swarm.Retired(this);CloseFromController();return;}
        if(state==0)
        {
            if(!airborne&&now>=nextFlightAt)
            {
                nextFlightAt=now+Rand(2.3,4.8);
                if(random.NextDouble()<.52){airborne=true;flightStart=now;flightDuration=Rand(.9,1.45);targetSpeed=Rand(135,220);desired=heading+Rand(-1.25,1.25);}
            }
            if(airborne&&now-flightStart>=flightDuration){airborne=false;nextFlightAt=now+Rand(2.5,5.5);targetSpeed=Rand(75,170);}
            nextTurn-=dt;if(nextTurn<=0)
            {
                double dx=goalX-x,dy=goalY-y,distanceToGoal=Math.Sqrt(dx*dx+dy*dy);if(!hasRoamGoal||distanceToGoal<100){PickRoamGoal();dx=goalX-x;dy=goalY-y;distanceToGoal=Math.Sqrt(dx*dx+dy*dy);}
                double vx=dx/Math.Max(1,distanceToGoal)+Rand(-.48,.48),vy=dy/Math.Max(1,distanceToGoal)+Rand(-.48,.48);
                foreach(RoachCloneWindow other in swarm.Roaches)
                {if(other==this||!other.IsRoaming)continue;double ox=x-other.WorldX,oy=y-other.WorldY,d2=ox*ox+oy*oy;if(d2>1&&d2<19600){double d=Math.Sqrt(d2),weight=(140-d)/140;vx+=ox/d*weight*2.6;vy+=oy/d*weight*2.6;}}
                if(x<area.Left+95)vx+=(area.Left+95-x)/95*1.8;if(x>area.Right-95)vx-=(x-(area.Right-95))/95*1.8;
                if(y<area.Top+95)vy+=(area.Top+95-y)/95*1.8;if(y>area.Bottom-95)vy-=(y-(area.Bottom-95))/95*1.8;
                desired=Math.Atan2(vy,vx)+Rand(-.42,.42);targetSpeed=airborne?Rand(135,220):Rand(75,180);nextTurn=Rand(.25,.8);
            }
            double margin=36,look=margin+speed*.3;
            if(x<area.Left+look||x>area.Right-look||y<area.Top+look||y>area.Bottom-look){if(x<area.Left+look)desired=Rand(-.9,.9);else if(x>area.Right-look)desired=Math.PI+Rand(-.9,.9);if(y<area.Top+look)desired=Rand(.25,2.9);else if(y>area.Bottom-look)desired=Rand(-2.9,-.25);targetSpeed=Math.Max(targetSpeed,airborne?145:88);PickRoamGoal();}
            double error=Wrap(desired-heading),maxTurn=3.6*dt;heading+=Math.Max(-maxTurn,Math.Min(maxTurn,error));
            speed+=(targetSpeed-speed)*Math.Min(1,dt*(targetSpeed>speed?2.7:3.5));double distance=speed*dt,oldX=x,oldY=y;
            x=Math.Max(area.Left+margin,Math.Min(area.Right-margin,x+Math.Cos(heading)*distance));y=Math.Max(area.Top+margin,Math.Min(area.Bottom-margin,y+Math.Sin(heading)*distance));
            phase=(phase+Math.Sqrt((x-oldX)*(x-oldX)+(y-oldY)*(y-oldY))/Math.Max(9,27*PetScale))%1;
        }
        Draw(now);
    }
    void Draw(double now)
    {
        Bitmap frame=state==1?rig.NymphCrush[Math.Min(rig.NymphCrush.Length-1,(int)(Math.Max(0,now-actionStart)*rig.NymphCrush.Length))]:airborne?rig.NymphPounce[Pet.SmallFlightFrame(now-flightStart,flightDuration)]:rig.Nymph[(int)(phase*rig.Nymph.Length)%rig.Nymph.Length];
        Graphics g=graphics;g.ResetTransform();g.Clear(Color.Transparent);
        {
            float lift=airborne?(float)Math.Sin(Math.PI*Math.Max(0,Math.Min(1,(now-flightStart)/Math.Max(.01,flightDuration))))*11:0;g.TranslateTransform(56,56-lift);g.RotateTransform((float)(heading*180/Math.PI+90));
            float size=256*PetScale;
            if(state==2)
            {
                float alpha=Math.Max(0,1-(float)((now-actionStart)/.38));ColorMatrix matrix=new ColorMatrix();matrix.Matrix33=alpha;
                using(ImageAttributes attributes=new ImageAttributes()){attributes.SetColorMatrix(matrix,ColorMatrixFlag.Default,ColorAdjustType.Bitmap);Rectangle destination=new Rectangle((int)Math.Round(-size/2),(int)Math.Round(-size/2),(int)Math.Ceiling(size),(int)Math.Ceiling(size));g.DrawImage(frame,destination,0,0,frame.Width,frame.Height,GraphicsUnit.Pixel,attributes);}
            }
            else g.DrawImage(frame,new RectangleF(-size/2,-size/2,size,size));
        }
        g.ResetTransform();
        Native.Show(Handle,buffer,(int)Math.Round(x)-56,(int)Math.Round(y)-56);
    }
    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);if(e.Button!=MouseButtons.Left||state!=0)return;
        DateTime now=DateTime.UtcNow;Size limit=SystemInformation.DoubleClickSize;
        if(lastClick!=DateTime.MinValue&&(now-lastClick).TotalMilliseconds<=SystemInformation.DoubleClickTime&&Math.Abs(e.X-previousClick.X)<=limit.Width&&Math.Abs(e.Y-previousClick.Y)<=limit.Height){lastClick=DateTime.MinValue;BeginCrush();return;}
        lastClick=now;previousClick=e.Location;
    }
    Point previousClick;
    protected override void OnFormClosed(FormClosedEventArgs e){closed=true;Native.Release(Handle);graphics.Dispose();buffer.Dispose();base.OnFormClosed(e);}
}

static class Native
{
    [StructLayout(LayoutKind.Sequential)] struct POINT{public int X,Y;public POINT(int x,int y){X=x;Y=y;}}
    [StructLayout(LayoutKind.Sequential)] struct SIZE{public int W,H;public SIZE(int w,int h){W=w;H=h;}}
    [StructLayout(LayoutKind.Sequential,Pack=1)] struct BLEND{public byte Op,Flags,Alpha,Format;}
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h,IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc,IntPtr obj);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
    [StructLayout(LayoutKind.Sequential)] struct INFO {public uint Size;public int Width,Height;public ushort Planes,Bits;public uint Compression,ImageSize;public int Xppm,Yppm;public uint Used,Important;}
    [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc,ref INFO info,uint usage,out IntPtr bits,IntPtr section,uint offset);
    [DllImport("user32.dll",SetLastError=true)] static extern bool UpdateLayeredWindow(IntPtr hwnd,IntPtr dst,ref POINT pos,ref SIZE size,IntPtr src,ref POINT origin,int color,ref BLEND blend,int flags);
    sealed class Surface
    {
        public int Width,Height;
        public IntPtr Memory,Bitmap,Bits,Previous;
        public byte[] Pixels;
        public void Dispose(){if(Memory!=IntPtr.Zero&&Previous!=IntPtr.Zero)SelectObject(Memory,Previous);if(Bitmap!=IntPtr.Zero)DeleteObject(Bitmap);if(Memory!=IntPtr.Zero)DeleteDC(Memory);Memory=Bitmap=Bits=Previous=IntPtr.Zero;Pixels=null;}
    }
    static readonly Dictionary<IntPtr,Surface> surfaces=new Dictionary<IntPtr,Surface>();
    static Surface CreateSurface(int width,int height)
    {
        IntPtr screen=GetDC(IntPtr.Zero);if(screen==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        IntPtr memory=CreateCompatibleDC(screen),bits=IntPtr.Zero,handle=IntPtr.Zero,old=IntPtr.Zero;
        try
        {
            if(memory==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            INFO info=new INFO{Size=40,Width=width,Height=-height,Planes=1,Bits=32};handle=CreateDIBSection(screen,ref info,0,out bits,IntPtr.Zero,0);
            if(handle==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            old=SelectObject(memory,handle);if(old==IntPtr.Zero||old==new IntPtr(-1))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return new Surface{Width=width,Height=height,Memory=memory,Bitmap=handle,Bits=bits,Previous=old,Pixels=new byte[width*height*4]};
        }
        catch{if(memory!=IntPtr.Zero){if(old!=IntPtr.Zero&&old!=new IntPtr(-1))SelectObject(memory,old);if(handle!=IntPtr.Zero)DeleteObject(handle);DeleteDC(memory);}throw;}
        finally{ReleaseDC(IntPtr.Zero,screen);}
    }
    public static void Release(IntPtr hwnd){Surface surface;if(surfaces.TryGetValue(hwnd,out surface)){surfaces.Remove(hwnd);surface.Dispose();}}
    public static void Show(IntPtr hwnd,Bitmap bitmap,int x,int y)
    {
        Surface surface;
        if(!surfaces.TryGetValue(hwnd,out surface)||surface.Width!=bitmap.Width||surface.Height!=bitmap.Height){Release(hwnd);surface=CreateSurface(bitmap.Width,bitmap.Height);surfaces[hwnd]=surface;}
        BitmapData data=bitmap.LockBits(new Rectangle(0,0,bitmap.Width,bitmap.Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppPArgb);
        try{int rowBytes=bitmap.Width*4;for(int row=0;row<bitmap.Height;row++)Marshal.Copy(IntPtr.Add(data.Scan0,row*data.Stride),surface.Pixels,row*rowBytes,rowBytes);}finally{bitmap.UnlockBits(data);}
        Marshal.Copy(surface.Pixels,0,surface.Bits,surface.Pixels.Length);
        IntPtr screen=GetDC(IntPtr.Zero);if(screen==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        try{POINT pos=new POINT(x,y),origin=new POINT(0,0);SIZE size=new SIZE(bitmap.Width,bitmap.Height);BLEND blend=new BLEND{Op=0,Flags=0,Alpha=255,Format=1};if(!UpdateLayeredWindow(hwnd,screen,ref pos,ref size,surface.Memory,ref origin,0,ref blend,2))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());}
        finally{ReleaseDC(IntPtr.Zero,screen);}
    }
}

sealed class CrackEffect : Form
{
    readonly Bitmap bitmap=new Bitmap(640,640,PixelFormat.Format32bppPArgb);
    readonly List<PointF[]> fractures=new List<PointF[]>();
    readonly List<bool> mainFractures=new List<bool>();
    Point screenCenter;
    public double StartedAt {get;private set;}
    protected override bool ShowWithoutActivation {get{return true;}}
    protected override CreateParams CreateParams {get{CreateParams p=base.CreateParams;p.ExStyle|=0x80000|0x80|0x08000000;return p;}}
    public CrackEffect(){FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;TopMost=true;Size=new Size(640,640);}
    protected override void WndProc(ref Message m){if(m.Msg==0x84){m.Result=new IntPtr(-1);return;}base.WndProc(ref m);}
    public void Show(int x,int y,double now)
    {
        screenCenter=new Point(x,y);StartedAt=now;BuildFractures(x,y);
        if(!Visible)base.Show();TopMost=true;Present(1,0);Program.Log("SCREEN IMPACT overlay at "+x+","+y+" visible="+Visible);
    }
    void BuildFractures(int x,int y)
    {
        fractures.Clear();mainFractures.Clear();Random random=new Random(unchecked(x*397^y*7919^(int)DateTime.UtcNow.Ticks));
        const float cx=320,cy=320;
        double baseAngle=random.NextDouble()*Math.PI*2;int rays=7+random.Next(2);
        for(int ray=0;ray<rays;ray++)
        {
            double angle=baseAngle+ray*Math.PI*2/rays+(random.NextDouble()-.5)*.38;float reach=(float)(115+random.NextDouble()*175);int pieces=6+random.Next(4);
            PointF[] points=new PointF[pieces+1];points[0]=new PointF(cx,cy);double drift=0;
            for(int n=1;n<=pieces;n++){drift+=(random.NextDouble()-.5)*.13;float radius=reach*n/pieces*(float)(.92+random.NextDouble()*.16);points[n]=new PointF(cx+(float)Math.Cos(angle+drift)*radius,cy+(float)Math.Sin(angle+drift)*radius);}
            fractures.Add(points);mainFractures.Add(true);
            int branches=2+random.Next(3);
            for(int b=0;b<branches;b++)
            {
                int originIndex=2+random.Next(Math.Max(1,pieces-3));PointF origin=points[originIndex];double branchAngle=angle+drift+(random.NextDouble()<.5?-1:1)*(0.4+random.NextDouble()*.85);float length=(float)(32+random.NextDouble()*86);int npoints=3+random.Next(3);PointF[] branch=new PointF[npoints];branch[0]=origin;double bend=0;
                for(int n=1;n<npoints;n++){bend+=(random.NextDouble()-.5)*.2;float r=length*n/(npoints-1);branch[n]=new PointF(origin.X+(float)Math.Cos(branchAngle+bend)*r,origin.Y+(float)Math.Sin(branchAngle+bend)*r);}
                fractures.Add(branch);mainFractures.Add(false);
            }
        }
        for(int ring=0;ring<2;ring++)
        {
            float radius=ring==0?35:72;PointF previous=PointF.Empty;int sides=9+ring*5;
            for(int i=0;i<=sides;i++){double a=baseAngle+i*Math.PI*2/sides;float r=radius*(float)(.82+random.NextDouble()*.34);PointF p=new PointF(cx+(float)Math.Cos(a)*r,cy+(float)Math.Sin(a)*r);if(i>0){fractures.Add(new PointF[]{previous,p});mainFractures.Add(false);}previous=p;}
        }
    }
    public void Update(float strength,double elapsed){if(!Visible)return;if(strength<=0){Hide();return;}Present(strength,elapsed);}
    void Present(float strength,double elapsed)
    {
        using(Graphics g=Graphics.FromImage(bitmap))
        {
            g.Clear(Color.Transparent);g.SmoothingMode=SmoothingMode.AntiAlias;
            float a=Math.Max(0,Math.Min(1,strength));
            for(int i=0;i<fractures.Count;i++)
            {
                PointF[] path=fractures[i];bool major=mainFractures[i];int darkAlpha=(int)((major?74:48)*a),lightAlpha=(int)((major?205:152)*a);
                using(Pen dark=new Pen(Color.FromArgb(darkAlpha,29,33,38),major?1.65f:.95f))g.DrawLines(dark,path);
                PointF[] bevel=new PointF[path.Length];for(int n=0;n<path.Length;n++)bevel[n]=new PointF(path[n].X+.65f,path[n].Y-.45f);
                using(Pen light=new Pen(Color.FromArgb(lightAlpha,224,237,246),major?.62f:.42f))g.DrawLines(light,bevel);
            }
            float flash=Math.Max(0,1-(float)(elapsed/.2));
            if(flash>0){using(Pen glint=new Pen(Color.FromArgb((int)(95*flash),247,251,255),1.1f))g.DrawEllipse(glint,320-17*flash,320-17*flash,34*flash,34*flash);}
        }
        Native.Show(Handle,bitmap,screenCenter.X-320,screenCenter.Y-320);
    }
    protected override void OnFormClosed(FormClosedEventArgs e){Native.Release(Handle);bitmap.Dispose();base.OnFormClosed(e);}
}
