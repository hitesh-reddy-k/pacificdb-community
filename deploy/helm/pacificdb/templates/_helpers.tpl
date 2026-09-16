{{- define "pacificdb.name" -}}
{{- printf "%s-pacificdb" .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "pacificdb.labels" -}}
app.kubernetes.io/name: pacificdb
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end -}}

{{- define "pacificdb.selectorLabels" -}}
app.kubernetes.io/name: pacificdb
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}
