// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <emscripten.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRhiWidget>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

// clang-format off
EM_JS(void, reportBrowserWebGl, (), {
  const gl = typeof GL !== 'undefined' && GL.currentContext ? GL.currentContext.GLctx : null;
  const version = gl ? gl.getParameter(gl.VERSION) : null;
  const result = {
    webgl2: typeof version === 'string' && version.startsWith('WebGL 2.0'),
    version,
    shading_language: gl ? gl.getParameter(gl.SHADING_LANGUAGE_VERSION) : null,
    max_samples: gl ? gl.getParameter(gl.MAX_SAMPLES) : 0,
    max_3d_texture_size: gl ? gl.getParameter(gl.MAX_3D_TEXTURE_SIZE) : 0,
    color_buffer_float: !!(gl && gl.getExtension('EXT_color_buffer_float')),
    float_linear: !!(gl && gl.getExtension('OES_texture_float_linear')),
    timer_query: !!(gl && gl.getExtension('EXT_disjoint_timer_query_webgl2')),
    cross_origin_isolated: globalThis.crossOriginIsolated === true,
    shared_array_buffer: typeof globalThis.SharedArrayBuffer === 'function',
    user_agent: navigator.userAgent,
  };
  console.log('PJ_WASM_SCENE3D_CAP_WEBGL ' + JSON.stringify(result));
});
// clang-format on

namespace {

using namespace Qt::StringLiterals;

QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning("Scene3D capability probe: cannot open shader %s", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

QByteArray solidSlice(const QColor& color) {
  QByteArray bytes(2 * 2 * 4, Qt::Uninitialized);
  auto* data = reinterpret_cast<quint8*>(bytes.data());
  for (int pixel = 0; pixel < 4; ++pixel) {
    const int offset = pixel * 4;
    data[offset + 0] = static_cast<quint8>(color.red());
    data[offset + 1] = static_cast<quint8>(color.green());
    data[offset + 2] = static_cast<quint8>(color.blue());
    data[offset + 3] = 255;
  }
  return bytes;
}

bool nearColor(const QColor& actual, const QColor& expected, int tolerance = 20) {
  return std::abs(actual.red() - expected.red()) <= tolerance &&
         std::abs(actual.green() - expected.green()) <= tolerance &&
         std::abs(actual.blue() - expected.blue()) <= tolerance && actual.alpha() >= 240;
}

qsizetype countColor(const QImage& image, const QColor& expected, const QRect& region = {}) {
  const QRect bounded = (region.isValid() ? region : image.rect()).intersected(image.rect());
  qsizetype count = 0;
  for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
    for (int x = bounded.left(); x <= bounded.right(); ++x) {
      if (nearColor(image.pixelColor(x, y), expected)) {
        ++count;
      }
    }
  }
  return count;
}

QJsonArray toJsonArray(const QList<int>& values) {
  QJsonArray out;
  for (const int value : values) {
    out.push_back(value);
  }
  return out;
}

class CapabilityWidget final : public QRhiWidget {
 public:
  CapabilityWidget(QString name, QColor volume_color, QColor float_color, QWidget* parent = nullptr)
      : QRhiWidget(parent),
        name_(std::move(name)),
        volume_color_(std::move(volume_color)),
        float_color_(std::move(float_color)),
        volume_slice_0_(solidSlice(QColor(12, 12, 12))),
        volume_slice_1_(solidSlice(volume_color_)) {
    // A WebGL2 capability probe by design, not PJ::preferredGraphicsApi().
    setApi(Api::OpenGL);
    setSampleCount(4);
    setMinimumSize(440, 440);
    setObjectName(u"scene3dCapability"_s + name_);
    connect(this, &QRhiWidget::renderFailed, this, [this]() {
      render_failed_ = true;
      qWarning("PJ_WASM_SCENE3D_CAP_RENDER_FAILED name=%s", qPrintable(name_));
    });
  }

  ~CapabilityWidget() override {
    releaseResources();
  }

  [[nodiscard]] int frameCount() const noexcept {
    return frame_count_;
  }

  [[nodiscard]] quintptr rhiIdentity() const noexcept {
    return rhi_identity_;
  }

  [[nodiscard]] QJsonObject matrix() const {
    QJsonObject object = matrix_;
    object.insert(u"name"_s, name_);
    object.insert(u"rhi"_s, QString::number(rhi_identity_, 16));
    object.insert(u"frames"_s, frame_count_);
    object.insert(u"render_failed"_s, render_failed_);
    object.insert(u"resources_ready"_s, resources_ready_);
    object.insert(u"float_sample_uses_rgba16f"_s, float_sample_uses_rgba16f_);
    return object;
  }

  [[nodiscard]] QJsonObject evidence() const {
    const QImage framebuffer = grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
    const qsizetype volume_pixels = countColor(framebuffer, volume_color_);
    const qsizetype float_pixels = countColor(framebuffer, float_color_);
    const QRect left_point_region(
        framebuffer.width() * 25 / 100, framebuffer.height() * 25 / 100, framebuffer.width() * 25 / 100,
        framebuffer.height() * 50 / 100);
    const QRect right_point_region(
        framebuffer.width() * 50 / 100, framebuffer.height() * 25 / 100, framebuffer.width() * 25 / 100,
        framebuffer.height() * 50 / 100);
    const qsizetype left_point_pixels = countColor(framebuffer, QColor(255, 0, 0), left_point_region) +
                                        countColor(framebuffer, QColor(0, 0, 255), left_point_region);
    const qsizetype right_point_pixels = countColor(framebuffer, QColor(255, 0, 0), right_point_region) +
                                         countColor(framebuffer, QColor(0, 0, 255), right_point_region);
    const qsizetype yellow = countColor(framebuffer, QColor(255, 255, 0));
    const bool ok = resources_ready_ && !render_failed_ && framebuffer.width() >= 400 && framebuffer.height() >= 400 &&
                    volume_pixels > 20000 && float_pixels > 20000 && left_point_pixels > 1000 &&
                    right_point_pixels > 1000 && yellow > 500;
    return {
        {u"name"_s, name_},
        {u"width"_s, framebuffer.width()},
        {u"height"_s, framebuffer.height()},
        {u"volume_pixels"_s, static_cast<qint64>(volume_pixels)},
        {u"vertex_stage_3d_texel_fetch"_s, volume_pixels > 20000},
        {u"float_pixels"_s, static_cast<qint64>(float_pixels)},
        {u"left_point_pixels"_s, static_cast<qint64>(left_point_pixels)},
        {u"right_point_pixels"_s, static_cast<qint64>(right_point_pixels)},
        {u"uint32_index_yellow"_s, static_cast<qint64>(yellow)},
        {u"ok"_s, ok},
    };
  }

 protected:
  void initialize(QRhiCommandBuffer* command_buffer) override {
    Q_UNUSED(command_buffer);
    QRhi* current_rhi = rhi();
    if (current_rhi == nullptr) {
      render_failed_ = true;
      return;
    }
    if (active_rhi_ != current_rhi || !resources_ready_) {
      releaseResources();
      active_rhi_ = current_rhi;
      rhi_identity_ = reinterpret_cast<quintptr>(current_rhi);
      resources_ready_ = createResources(current_rhi);
    }
    if (renderTarget() != nullptr) {
      matrix_.insert(u"actual_sample_count"_s, renderTarget()->sampleCount());
    }
  }

  void render(QRhiCommandBuffer* command_buffer) override {
    QRhi* current_rhi = rhi();
    QRhiRenderTarget* target = renderTarget();
    if (current_rhi == nullptr || target == nullptr) {
      render_failed_ = true;
      return;
    }

    QRhiResourceUpdateBatch* updates = current_rhi->nextResourceUpdateBatch();
    if (upload_pending_ && resources_ready_) {
      updates->uploadStaticBuffer(point_vertex_buffer_, point_vertex_.data());
      updates->uploadStaticBuffer(indexed_vertex_buffer_, indexed_vertices_.data());
      updates->uploadStaticBuffer(index_buffer_, indices_.data());

      QRhiTextureSubresourceUploadDescription slice_0(volume_slice_0_);
      slice_0.setSourceSize(QSize(2, 2));
      QRhiTextureSubresourceUploadDescription slice_1(volume_slice_1_);
      slice_1.setSourceSize(QSize(2, 2));
      updates->uploadTexture(
          volume_texture_,
          QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, slice_0), QRhiTextureUploadEntry(1, 0, slice_1)}));
      upload_pending_ = false;
    }

    if (!resources_ready_) {
      command_buffer->beginPass(target, QColor(24, 24, 24), {1.0F, 0}, updates);
      command_buffer->endPass();
      return;
    }

    command_buffer->beginPass(float_render_target_, float_color_, {1.0F, 0}, updates);
    command_buffer->endPass();

    static bool browser_reported = false;
    if (!browser_reported) {
      reportBrowserWebGl();
      browser_reported = true;
    }

    const QSize output_size = target->pixelSize();
    const QRhiViewport viewport(
        0.0F, 0.0F, static_cast<float>(output_size.width()), static_cast<float>(output_size.height()));
    command_buffer->beginPass(target, QColor(0, 0, 0), {1.0F, 0});

    command_buffer->setGraphicsPipeline(background_pipeline_);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(background_srb_);
    command_buffer->draw(3);

    command_buffer->setGraphicsPipeline(point_pipeline_);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(empty_srb_);
    const QRhiCommandBuffer::VertexInput point_input(point_vertex_buffer_, 0);
    command_buffer->setVertexInput(0, 1, &point_input);
    command_buffer->draw(1, 2);

    command_buffer->setGraphicsPipeline(indexed_pipeline_);
    command_buffer->setViewport(viewport);
    command_buffer->setShaderResources(empty_srb_);
    const QRhiCommandBuffer::VertexInput indexed_input(indexed_vertex_buffer_, 0);
    command_buffer->setVertexInput(0, 1, &indexed_input, index_buffer_, 0, QRhiCommandBuffer::IndexUInt32);
    command_buffer->drawIndexed(3);
    command_buffer->endPass();

    ++frame_count_;
    if (frame_count_ < 4) {
      update();
    }
  }

  void releaseResources() override {
    delete background_pipeline_;
    delete point_pipeline_;
    delete indexed_pipeline_;
    delete background_srb_;
    delete empty_srb_;
    delete volume_sampler_;
    delete float_sampler_;
    delete float_render_target_;
    delete float_render_pass_;
    delete volume_texture_;
    delete float_texture_;
    delete point_vertex_buffer_;
    delete indexed_vertex_buffer_;
    delete index_buffer_;
    background_pipeline_ = nullptr;
    point_pipeline_ = nullptr;
    indexed_pipeline_ = nullptr;
    background_srb_ = nullptr;
    empty_srb_ = nullptr;
    volume_sampler_ = nullptr;
    float_sampler_ = nullptr;
    float_render_target_ = nullptr;
    float_render_pass_ = nullptr;
    volume_texture_ = nullptr;
    float_texture_ = nullptr;
    point_vertex_buffer_ = nullptr;
    indexed_vertex_buffer_ = nullptr;
    index_buffer_ = nullptr;
    active_rhi_ = nullptr;
    resources_ready_ = false;
    upload_pending_ = true;
  }

 private:
  static QJsonObject probeRenderTarget(QRhi* rhi, QRhiTexture::Format format) {
    QJsonObject result;
    result.insert(u"advertised"_s, rhi->isTextureFormatSupported(format, QRhiTexture::RenderTarget));
    QRhiTexture* texture = rhi->newTexture(format, QSize(8, 8), 1, QRhiTexture::RenderTarget);
    const bool texture_created = texture->create();
    bool target_created = false;
    QRhiTextureRenderTarget* target = nullptr;
    QRhiRenderPassDescriptor* render_pass = nullptr;
    if (texture_created) {
      target = rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(texture));
      render_pass = target->newCompatibleRenderPassDescriptor();
      target->setRenderPassDescriptor(render_pass);
      target_created = target->create();
    }
    result.insert(u"texture"_s, texture_created);
    result.insert(u"render_target"_s, target_created);
    delete target;
    delete render_pass;
    delete texture;
    return result;
  }

  bool createResources(QRhi* rhi) {
    QJsonObject features;
    const auto add_feature = [rhi, &features](QStringView name, QRhi::Feature feature) {
      features.insert(name.toString(), rhi->isFeatureSupported(feature));
    };
    add_feature(u"multisample_texture", QRhi::MultisampleTexture);
    add_feature(u"multisample_renderbuffer", QRhi::MultisampleRenderBuffer);
    add_feature(u"instancing", QRhi::Instancing);
    add_feature(u"element_index_uint", QRhi::ElementIndexUint);
    add_feature(u"compute", QRhi::Compute);
    add_feature(u"vertex_shader_point_size", QRhi::VertexShaderPointSize);
    add_feature(u"texel_fetch", QRhi::TexelFetch);
    add_feature(u"int_attributes", QRhi::IntAttributes);
    add_feature(u"three_dimensional_textures", QRhi::ThreeDimensionalTextures);
    add_feature(u"render_to_3d_texture_slice", QRhi::RenderTo3DTextureSlice);
    add_feature(u"texture_arrays", QRhi::TextureArrays);
    add_feature(u"geometry_shader", QRhi::GeometryShader);
    add_feature(u"tessellation", QRhi::Tessellation);
    add_feature(u"non_fill_polygon_mode", QRhi::NonFillPolygonMode);
    matrix_.insert(u"features"_s, features);
    matrix_.insert(u"supported_sample_counts"_s, toJsonArray(rhi->supportedSampleCounts()));

    QJsonObject float_targets;
    float_targets.insert(u"rgba16f"_s, probeRenderTarget(rhi, QRhiTexture::RGBA16F));
    float_targets.insert(u"rgba32f"_s, probeRenderTarget(rhi, QRhiTexture::RGBA32F));
    float_targets.insert(u"r16f"_s, probeRenderTarget(rhi, QRhiTexture::R16F));
    float_targets.insert(u"r32f"_s, probeRenderTarget(rhi, QRhiTexture::R32F));
    matrix_.insert(u"float_targets"_s, float_targets);

    const bool core_features = features.value(u"instancing"_s).toBool() &&
                               features.value(u"element_index_uint"_s).toBool() &&
                               features.value(u"vertex_shader_point_size"_s).toBool() &&
                               features.value(u"three_dimensional_textures"_s).toBool();

    volume_texture_ = rhi->newTexture(QRhiTexture::RGBA8, 2, 2, 2, 1, QRhiTexture::ThreeDimensional);
    const bool volume_created = volume_texture_->create();
    matrix_.insert(u"volume_texture_created"_s, volume_created);

    float_sample_uses_rgba16f_ = float_targets.value(u"rgba16f"_s).toObject().value(u"render_target"_s).toBool();
    const QRhiTexture::Format sample_format = float_sample_uses_rgba16f_ ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    float_texture_ = rhi->newTexture(sample_format, QSize(8, 8), 1, QRhiTexture::RenderTarget);
    bool float_texture_created = float_texture_->create();
    if (float_texture_created) {
      float_render_target_ = rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(float_texture_));
      float_render_pass_ = float_render_target_->newCompatibleRenderPassDescriptor();
      float_render_target_->setRenderPassDescriptor(float_render_pass_);
      float_texture_created = float_render_target_->create();
    }
    matrix_.insert(u"sample_float_target_created"_s, float_texture_created);

    volume_sampler_ = rhi->newSampler(
        QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    const bool volume_sampler_created = volume_sampler_->create();
    float_sampler_ = rhi->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge);
    const bool float_sampler_created = float_sampler_->create();

    point_vertex_buffer_ =
        rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(sizeof(point_vertex_)));
    indexed_vertex_buffer_ = rhi->newBuffer(
        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(sizeof(indexed_vertices_)));
    index_buffer_ =
        rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, static_cast<quint32>(sizeof(indices_)));
    const bool buffers_created =
        point_vertex_buffer_->create() && indexed_vertex_buffer_->create() && index_buffer_->create();

    background_srb_ = rhi->newShaderResourceBindings();
    background_srb_->setBindings({
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::VertexStage, volume_texture_, volume_sampler_),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, float_texture_, float_sampler_),
    });
    const bool background_srb_created = background_srb_->create();
    empty_srb_ = rhi->newShaderResourceBindings();
    empty_srb_->setBindings({});
    const bool empty_srb_created = empty_srb_->create();

    const QShader background_vert = loadShader(u":/scene3d_capability/background.vert.qsb"_s);
    const QShader background_frag = loadShader(u":/scene3d_capability/background.frag.qsb"_s);
    const QShader point_vert = loadShader(u":/scene3d_capability/point.vert.qsb"_s);
    const QShader point_frag = loadShader(u":/scene3d_capability/point.frag.qsb"_s);
    const QShader indexed_vert = loadShader(u":/scene3d_capability/indexed.vert.qsb"_s);
    const QShader indexed_frag = loadShader(u":/scene3d_capability/indexed.frag.qsb"_s);
    const bool shaders_valid = background_vert.isValid() && background_frag.isValid() && point_vert.isValid() &&
                               point_frag.isValid() && indexed_vert.isValid() && indexed_frag.isValid();

    background_pipeline_ = rhi->newGraphicsPipeline();
    background_pipeline_->setShaderStages({
        QRhiShaderStage(QRhiShaderStage::Vertex, background_vert),
        QRhiShaderStage(QRhiShaderStage::Fragment, background_frag),
    });
    background_pipeline_->setShaderResourceBindings(background_srb_);
    background_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    const bool background_pipeline_created = background_pipeline_->create();

    QRhiVertexInputLayout position_layout;
    position_layout.setBindings({QRhiVertexInputBinding(static_cast<quint32>(2 * sizeof(float)))});
    position_layout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
    });

    point_pipeline_ = rhi->newGraphicsPipeline();
    point_pipeline_->setShaderStages({
        QRhiShaderStage(QRhiShaderStage::Vertex, point_vert),
        QRhiShaderStage(QRhiShaderStage::Fragment, point_frag),
    });
    point_pipeline_->setTopology(QRhiGraphicsPipeline::Points);
    point_pipeline_->setVertexInputLayout(position_layout);
    point_pipeline_->setShaderResourceBindings(empty_srb_);
    point_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    const bool point_pipeline_created = point_pipeline_->create();

    indexed_pipeline_ = rhi->newGraphicsPipeline();
    indexed_pipeline_->setShaderStages({
        QRhiShaderStage(QRhiShaderStage::Vertex, indexed_vert),
        QRhiShaderStage(QRhiShaderStage::Fragment, indexed_frag),
    });
    indexed_pipeline_->setVertexInputLayout(position_layout);
    indexed_pipeline_->setShaderResourceBindings(empty_srb_);
    indexed_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    const bool indexed_pipeline_created = indexed_pipeline_->create();

    const bool ok = core_features && volume_created && float_texture_created && volume_sampler_created &&
                    float_sampler_created && buffers_created && background_srb_created && empty_srb_created &&
                    shaders_valid && background_pipeline_created && point_pipeline_created && indexed_pipeline_created;
    matrix_.insert(u"resource_setup_ok"_s, ok);
    return ok;
  }

  QString name_;
  QColor volume_color_;
  QColor float_color_;
  QByteArray volume_slice_0_;
  QByteArray volume_slice_1_;
  std::array<float, 2> point_vertex_ = {0.0F, 0.0F};
  std::array<float, 6> indexed_vertices_ = {
      -0.88F, -0.78F, -0.52F, -0.78F, -0.70F, -0.38F,
  };
  std::array<quint32, 3> indices_ = {0, 1, 2};
  QRhi* active_rhi_ = nullptr;
  quintptr rhi_identity_ = 0;
  QRhiGraphicsPipeline* background_pipeline_ = nullptr;
  QRhiGraphicsPipeline* point_pipeline_ = nullptr;
  QRhiGraphicsPipeline* indexed_pipeline_ = nullptr;
  QRhiShaderResourceBindings* background_srb_ = nullptr;
  QRhiShaderResourceBindings* empty_srb_ = nullptr;
  QRhiSampler* volume_sampler_ = nullptr;
  QRhiSampler* float_sampler_ = nullptr;
  QRhiTexture* volume_texture_ = nullptr;
  QRhiTexture* float_texture_ = nullptr;
  QRhiTextureRenderTarget* float_render_target_ = nullptr;
  QRhiRenderPassDescriptor* float_render_pass_ = nullptr;
  QRhiBuffer* point_vertex_buffer_ = nullptr;
  QRhiBuffer* indexed_vertex_buffer_ = nullptr;
  QRhiBuffer* index_buffer_ = nullptr;
  QJsonObject matrix_;
  int frame_count_ = 0;
  bool upload_pending_ = true;
  bool resources_ready_ = false;
  bool render_failed_ = false;
  bool float_sample_uses_rgba16f_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  Q_INIT_RESOURCE(scene3d_capability_shaders);
  QApplication application(argc, argv);
  application.setApplicationName(u"PJ4 Scene3D WASM Capability Probe"_s);

  QWidget window;
  window.setWindowTitle(u"PJ4 Scene3D WebAssembly capability matrix"_s);
  auto* layout = new QVBoxLayout(&window);
  auto* title = new QLabel(
      u"Retained W12 probe — two live QRhiWidgets, vertex-stage 3D texelFetch + float target, "
      "instanced point sprites, and UInt32 indexed geometry"_s,
      &window);
  title->setWordWrap(true);
  layout->addWidget(title);

  auto* splitter = new QSplitter(Qt::Horizontal, &window);
  auto* widget_a = new CapabilityWidget(u"A"_s, QColor(0, 180, 40), QColor(230, 110, 20), splitter);
  auto* widget_b = new CapabilityWidget(u"B"_s, QColor(180, 20, 180), QColor(20, 170, 230), splitter);
  splitter->addWidget(widget_a);
  splitter->addWidget(widget_b);
  splitter->setSizes({560, 560});
  layout->addWidget(splitter, 1);
  window.resize(1200, 700);
  window.show();

  auto* report_timer = new QTimer(&window);
  report_timer->setInterval(100);
  QObject::connect(report_timer, &QTimer::timeout, &window, [report_timer, widget_a, widget_b, attempts = 0]() mutable {
    ++attempts;
    if (widget_a->frameCount() < 3 || widget_b->frameCount() < 3) {
      if (attempts < 300) {
        return;
      }
    }

    const QJsonObject evidence_a = widget_a->evidence();
    const QJsonObject evidence_b = widget_b->evidence();
    const bool same_rhi = widget_a->rhiIdentity() != 0 && widget_a->rhiIdentity() == widget_b->rhiIdentity();
    const bool ok = same_rhi && evidence_a.value(u"ok"_s).toBool() && evidence_b.value(u"ok"_s).toBool();
    const QJsonObject complete = {
        {u"schema"_s, 1},
        {u"same_rhi"_s, same_rhi},
        {u"widgets"_s, QJsonArray({widget_a->matrix(), widget_b->matrix()})},
        {u"evidence"_s, QJsonArray({evidence_a, evidence_b})},
        {u"ok"_s, ok},
    };
    qInfo().noquote() << "PJ_WASM_SCENE3D_CAP_COMPLETE" << QJsonDocument(complete).toJson(QJsonDocument::Compact);
    report_timer->stop();
  });
  report_timer->start();

  return application.exec();
}
