// One PDF page per JPEG, embedding each JPEG unchanged (DCTDecode). A4 width, image aspect ratio.
// Equivalent to mobile/src/pdf.c so a document's PDF looks the same offline and online.

const A4_WIDTH_PT = 595.28;

export function jpegInfo(buf) {
  if (buf.length < 4 || buf[0] !== 0xff || buf[1] !== 0xd8) return null;
  let i = 2;
  while (i + 9 < buf.length) {
    if (buf[i] !== 0xff) return null;
    const marker = buf[i + 1];
    if (marker === 0xff) { i += 1; continue; }
    if (marker === 0xd8 || (marker >= 0xd0 && marker <= 0xd7)) { i += 2; continue; }
    const seg = buf.readUInt16BE(i + 2);
    if (marker >= 0xc0 && marker <= 0xcf && ![0xc4, 0xc8, 0xcc].includes(marker)) {
      const height = buf.readUInt16BE(i + 5);
      const width = buf.readUInt16BE(i + 7);
      return width && height ? { width, height, components: buf[i + 9] } : null;
    }
    if (marker === 0xda) return null;
    i += 2 + seg;
  }
  return null;
}

export function pdfFromJpegs(jpegs) {
  const chunks = [];
  let length = 0;
  const offsets = [];
  const write = (data) => {
    const b = typeof data === 'string' ? Buffer.from(data, 'latin1') : data;
    chunks.push(b);
    length += b.length;
  };

  write('%PDF-1.4\n%\xE2\xE3\xCF\xD3\n');
  let next = 3;
  const pageObjs = [];
  jpegs.forEach((jpeg) => {
    const info = jpegInfo(jpeg);
    if (!info) return;
    const cs = info.components === 1 ? '/DeviceGray' : info.components === 4 ? '/DeviceCMYK' : '/DeviceRGB';
    const w = A4_WIDTH_PT.toFixed(2);
    const h = ((A4_WIDTH_PT * info.height) / info.width).toFixed(2);
    const [page, content, image] = [next, next + 1, next + 2];
    next += 3;
    const n = pageObjs.length;
    offsets[page] = length;
    write(`${page} 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${w} ${h}] /Resources << /XObject << /Im${n} ${image} 0 R >> >> /Contents ${content} 0 R >>\nendobj\n`);
    const stream = `q ${w} 0 0 ${h} 0 0 cm /Im${n} Do Q\n`;
    offsets[content] = length;
    write(`${content} 0 obj\n<< /Length ${stream.length} >>\nstream\n${stream}\nendstream\nendobj\n`);
    offsets[image] = length;
    const decode = info.components === 4 ? ' /Decode [1 0 1 0 1 0 1 0]' : '';
    write(`${image} 0 obj\n<< /Type /XObject /Subtype /Image /Width ${info.width} /Height ${info.height} /ColorSpace ${cs} /BitsPerComponent 8 /Filter /DCTDecode${decode} /Length ${jpeg.length} >>\nstream\n`);
    write(jpeg);
    write('\nendstream\nendobj\n');
    pageObjs.push(page);
  });
  if (pageObjs.length === 0) return null;

  offsets[1] = length;
  write('1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n');
  offsets[2] = length;
  write(`2 0 obj\n<< /Type /Pages /Count ${pageObjs.length} /Kids [${pageObjs.map((p) => `${p} 0 R`).join(' ')}] >>\nendobj\n`);
  const xref = length;
  let table = `xref\n0 ${next}\n0000000000 65535 f \n`;
  for (let i = 1; i < next; i += 1) table += `${String(offsets[i]).padStart(10, '0')} 00000 n \n`;
  write(table);
  write(`trailer\n<< /Size ${next} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`);
  return { pdf: Buffer.concat(chunks), pages: pageObjs.length };
}
