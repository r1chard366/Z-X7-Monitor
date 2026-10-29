using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

// 把白底图标转成透明底，并重新打包成多尺寸 ICO（托盘用得上）
public static class IcoTransparent
{
    public static void Convert(string srcIco, string dstIco)
    {
        int[] sizes = { 16, 24, 32, 48, 64, 128, 256 };
        var images = new List<byte[]>();

        using (var original = new Bitmap(srcIco))
        {
            // 1) 先把白色背景变透明
            var flat = new Bitmap(original.Width, original.Height, PixelFormat.Format32bppArgb);
            for (int y = 0; y < original.Height; y++)
            {
                for (int x = 0; x < original.Width; x++)
                {
                    Color c = original.GetPixel(x, y);
                    int min = Math.Min(c.R, Math.Min(c.G, c.B));
                    int alpha;
                    if (min >= 235) alpha = 0;                       // 纯白 → 透明
                    else if (min >= 200) alpha = (235 - min) * 255 / 35; // 边缘半透明
                    else alpha = 255;
                    flat.SetPixel(x, y, Color.FromArgb(alpha, c.R, c.G, c.B));
                }
            }

            foreach (int s in sizes)
            {
                using (var bmp = new Bitmap(s, s, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(bmp))
                    {
                        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                        g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
                        g.DrawImage(flat, 0, 0, s, s);
                    }
                    if (s >= 128)
                    {
                        using (var ms = new MemoryStream()) { bmp.Save(ms, ImageFormat.Png); images.Add(ms.ToArray()); }
                    }
                    else
                    {
                        images.Add(ToDib(bmp));
                    }
                }
            }
            flat.Dispose();
        }

        using (var fs = new FileStream(dstIco, FileMode.Create))
        {
            var bw = new BinaryWriter(fs);
            bw.Write((ushort)0); bw.Write((ushort)1); bw.Write((ushort)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++)
            {
                int s = sizes[i];
                bw.Write((byte)(s >= 256 ? 0 : s));
                bw.Write((byte)(s >= 256 ? 0 : s));
                bw.Write((byte)0); bw.Write((byte)0);
                bw.Write((ushort)1); bw.Write((ushort)32);
                bw.Write(images[i].Length);
                bw.Write(offset);
                offset += images[i].Length;
            }
            foreach (var img in images) bw.Write(img);
        }
    }

    static byte[] ToDib(Bitmap bmp)
    {
        int w = bmp.Width, h = bmp.Height;
        int maskStride = ((w + 31) / 32) * 4;
        var ms = new MemoryStream();
        var bw = new BinaryWriter(ms);
        bw.Write(40); bw.Write(w); bw.Write(h * 2);
        bw.Write((ushort)1); bw.Write((ushort)32);
        bw.Write(0);
        bw.Write(w * h * 4 + maskStride * h);
        bw.Write(0); bw.Write(0); bw.Write(0); bw.Write(0);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++)
            {
                Color c = bmp.GetPixel(x, y);
                bw.Write(c.B); bw.Write(c.G); bw.Write(c.R); bw.Write(c.A);
            }
        // AND mask：alpha=0 的像素记为透明
        var mask = new byte[maskStride * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                Color c = bmp.GetPixel(x, y);
                if (c.A < 128)
                {
                    int row = h - 1 - y;
                    mask[row * maskStride + (x / 8)] |= (byte)(0x80 >> (x % 8));
                }
            }
        bw.Write(mask);
        bw.Flush();
        return ms.ToArray();
    }
}
