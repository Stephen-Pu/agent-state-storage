{{/* Common labels applied to every kvcache resource. */}}
{{- define "kvcache.labels" -}}
app.kubernetes.io/name: kvcache
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
helm.sh/chart: {{ .Chart.Name }}-{{ .Chart.Version | replace "+" "_" }}
{{- end }}

{{- define "kvcache.nodeImage" -}}
{{ .Values.image.registry }}/{{ .Values.image.node.repository }}:{{ .Values.image.node.tag }}
{{- end }}

{{- define "kvcache.cpImage" -}}
{{ .Values.image.registry }}/{{ .Values.image.cp.repository }}:{{ .Values.image.cp.tag }}
{{- end }}

{{- define "kvcache.operatorImage" -}}
{{ .Values.image.registry }}/{{ .Values.image.operator.repository }}:{{ .Values.image.operator.tag }}
{{- end }}

{{- define "kvcache.etcdEndpoints" -}}
{{- if .Values.etcd.byoEtcd -}}
{{- join "," .Values.etcd.endpoints -}}
{{- else -}}
{{- range $i, $_ := until (int .Values.etcd.replicaCount) -}}
{{- if $i -}},{{- end -}}{{ printf "%s-etcd-%d.%s-etcd:2379" $.Release.Name $i $.Release.Name -}}
{{- end -}}
{{- end -}}
{{- end }}
