import { NextFunction, Request, Response } from 'express'
import type { Command, PowerStatus } from '@prisma/client'
import { prisma } from '@/config/database'
import { env } from '@/config/env'
import { IngestSchema, type IngestDTO } from '../dto/ingest.dto'

const DEFAULT_GAS_URL =
  'https://script.google.com/macros/s/AKfycbwrWPH7UjAHjCwxnJb2kt2V68bjCvW9to0z9QsTrjRlxvbtH8YDJxFCTlRIyCBwTO4o/exec'

function forwardToGoogleSheets(body: IngestDTO) {
  const gasUrl = env.GAS_URL || DEFAULT_GAS_URL
  if (!gasUrl) return

  const statusName = body.status.charAt(0).toUpperCase() + body.status.slice(1).toLowerCase()
  const sleepMin = Math.round((body.sleep_s ?? 900) / 60)
  const predV = (body.predicted_v ?? body.vcap).toFixed(2)
  const infoText = `${statusName} (${sleepMin} menit) | Prediction: ${predV}V`

  const query = new URLSearchParams({
    v: body.vcap.toFixed(2),
    l: '100',
    s: '100',
    info: infoText,
  })

  fetch(`${gasUrl}?${query.toString()}`, { redirect: 'follow' })
    .then(async (res) => {
      const txt = await res.text()
      console.log(`[GoogleSheets] Synced ${body.vcap.toFixed(2)}V -> ${txt.trim()}`)
    })
    .catch((err) => {
      console.error('[GoogleSheets] Forward error:', err.message)
    })
}

function toDeviceReply(cmd: Command | null) {
  if (!cmd) return { ok: true, cmd: 'none' }
  if (cmd.type === 'RESTART') return { ok: true, cmd: 'restart' }
  if (cmd.type === 'SET_SLEEP') {
    const sleepS = (cmd.payload as { sleepS?: number } | null)?.sleepS ?? 900
    return { ok: true, cmd: 'sleep', sleep_s: sleepS }
  }
  if (cmd.type === 'UPDATE_WIFI') {
    const p = cmd.payload as { ssid?: string; pass?: string } | null
    return {
      ok: true,
      cmd: 'update_wifi',
      ssid: p?.ssid ?? '',
      pass: p?.pass ?? '',
    }
  }
  return { ok: true, cmd: 'none' }
}

export class IngestController {
  // Dibuat tipis: validasi, simpan, ambil perintah, balas. Tidak ada kerja berat,
  // karena setiap detik ESP32 menunggu adalah energi supercap.
  static async receive(req: Request, res: Response, next: NextFunction) {
    try {
      const body = IngestSchema.parse(req.body)
      const device = req.device!
      const now = new Date()

      const cmd = await prisma.$transaction(async (tx: any) => {
        await tx.reading.create({
          data: {
            deviceId: device.id,
            receivedAt: now,
            vcap: body.vcap,
            status: body.status.toUpperCase() as PowerStatus,
            predictedV: body.predicted_v,
            sleepS: body.sleep_s,
            resetReason: body.reset_reason,
            bootCount: body.boot_count,
            wifiMs: body.wifi_ms,
            activeMs: body.active_ms,
            wifiSsid: body.ssid,
          },
        })

        await tx.device.update({
          where: { id: device.id },
          data: {
            lastSeenAt: now,
            ...(body.ssid ? { wifiSsid: body.ssid } : {}),
          },
        })

        // Perintah yang sudah lewat masa berlakunya tidak boleh terkirim
        await tx.command.updateMany({
          where: { deviceId: device.id, status: 'PENDING', expiresAt: { lte: now } },
          data: { status: 'EXPIRED' },
        })

        const pending = await tx.command.findFirst({
          where: { deviceId: device.id, status: 'PENDING' },
          orderBy: { createdAt: 'asc' },
        })

        if (pending) {
          await tx.command.update({
            where: { id: pending.id },
            data: { status: 'DELIVERED', deliveredAt: now },
          })
        }
        return pending
      })

      res.status(200).json(toDeviceReply(cmd))
      forwardToGoogleSheets(body)
      return
    } catch (error) {
      next(error)
    }
  }
}
