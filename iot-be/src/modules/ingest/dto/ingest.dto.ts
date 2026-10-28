import { z } from 'zod'

export const IngestSchema = z.object({
  device_id: z.string().min(1).max(64),
  vcap: z.number().min(0).max(10, 'vcap di luar rentang wajar'),
  status: z.enum(['safe', 'medium', 'critical']),
  predicted_v: z.number().min(0).max(10).optional(),
  sleep_s: z.number().int().min(0).max(86400).optional(),
  reset_reason: z.string().max(32).optional(),
  boot_count: z.number().int().min(0).optional(),
  wifi_ms: z.number().int().min(0).max(120000).optional(),
  active_ms: z.number().int().min(0).max(600000).optional(),
  ssid: z.string().max(64).optional(),
})

export type IngestDTO = z.infer<typeof IngestSchema>
